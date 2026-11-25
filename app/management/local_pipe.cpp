#include "local_pipe.hpp"
#include "../software_log.hpp"
#include "protocol.hpp"
#include <windows.h>
#include <sddl.h>
#include <algorithm>
#include <cstdint>
#include <system_error>
#include <vector>

namespace dfy::management {
namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
struct CloseHandleDeleter {
    // 归还当前调用独占的 Windows 句柄。
    void operator()(void* handle) const noexcept { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void,CloseHandleDeleter>;

// 以原始系统错误报告连接、鉴权或传输失败。
[[noreturn]] void fail(DWORD error, const char* operation) {
    throw std::system_error(static_cast<int>(error),std::system_category(),operation);
}

// 校验 Windows 返回的句柄并接管其寿命。
Handle own(HANDLE handle, const char* operation) {
    if (!handle || handle == INVALID_HANDLE_VALUE) fail(GetLastError(),operation);
    return Handle(handle);
}

// 复制指定进程的用户 SID，仅用于本机端点所有者与连接对端的身份核对。
// 步骤：打开进程令牌 → 读取有界用户信息 → 转成独立的 SID 字符串。
std::wstring user_sid(HANDLE process) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process,TOKEN_QUERY,&raw)) fail(GetLastError(),"management token");
    auto token = own(raw,"management token");
    DWORD size = 0;
    GetTokenInformation(token.get(),TokenUser,nullptr,0,&size);
    if (!size || size>64*1024) fail(ERROR_INVALID_DATA,"management token size");
    std::vector<unsigned char> data(size);
    if (!GetTokenInformation(token.get(),TokenUser,data.data(),size,&size)) fail(GetLastError(),"management token user");

    LPWSTR value = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&value))
        fail(GetLastError(),"management user SID");
    std::unique_ptr<wchar_t,decltype(&LocalFree)> release(value,&LocalFree);
    return value;
}

// 用目录的卷号与文件编号归并路径别名，同一 Windows 用户共享该端点。
std::wstring pipe_name(const std::filesystem::path& profile, const std::wstring& sid) {
    auto directory = own(CreateFileW(profile.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr),"management profile");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(directory.get(),&info)) fail(GetLastError(),"management profile identity");
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) fail(ERROR_DIRECTORY,"management profile");
    return L"\\\\.\\pipe\\DaFeiYu.Management.1."+sid+L"."+std::to_wstring(info.dwVolumeSerialNumber)+
        L"."+std::to_wstring(info.nFileIndexHigh)+L"."+std::to_wstring(info.nFileIndexLow);
}

// 将剩余绝对期限转换为 Windows 等待时长，超期时立即返回。
DWORD remaining(Deadline deadline) {
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(deadline-Clock::now()).count();
    return static_cast<DWORD>(std::clamp<long long>(ms,0,MAXDWORD-1));
}

// 完成一次重叠 I/O，超时后等待取消实退，再释放调用栈上的 OVERLAPPED。
// 步骤：提交操作 → 等待仍在进行的请求 → 超时撤销或取得已完成字节数。
template<class Submit>
DWORD transfer(HANDLE pipe, Deadline deadline, Submit submit) {
    OVERLAPPED pending{};
    auto event = own(CreateEventW(nullptr,TRUE,FALSE,nullptr),"management I/O event");
    pending.hEvent = event.get();
    DWORD count = 0;
    if (submit(&count,&pending)) return count;
    const auto error = GetLastError();
    if (error != ERROR_IO_PENDING) fail(error,"management pipe I/O");

    const auto wait = WaitForSingleObject(event.get(),remaining(deadline));
    if (wait != WAIT_OBJECT_0) {
        const auto reason = wait == WAIT_TIMEOUT ? ERROR_SEM_TIMEOUT : GetLastError();
        CancelIoEx(pipe,&pending);
        if (GetOverlappedResult(pipe,&pending,&count,TRUE)) return count;
        fail(reason,"management pipe wait");
    }
    if (!GetOverlappedResult(pipe,&pending,&count,FALSE)) fail(GetLastError(),"management pipe completion");
    return count;
}

// 在同一期限内收齐指定字节数，短读沿当前位置继续。
void read_all(HANDLE pipe, void* data, std::size_t size, Deadline deadline) {
    auto* cursor = static_cast<char*>(data);
    while (size) {
        const auto length = static_cast<DWORD>(std::min<std::size_t>(size,64*1024));
        const auto count = transfer(pipe,deadline,[&](DWORD* n, OVERLAPPED* op) { return ReadFile(pipe,cursor,length,n,op); });
        if (!count) fail(ERROR_BROKEN_PIPE,"management pipe read");
        cursor += count;
        size -= count;
    }
}

// 在同一期限内交付完整字节序列，已写部分不重复发送。
void write_all(HANDLE pipe, const void* data, std::size_t size, Deadline deadline) {
    auto* cursor = static_cast<const char*>(data);
    while (size) {
        const auto length = static_cast<DWORD>(std::min<std::size_t>(size,64*1024));
        const auto count = transfer(pipe,deadline,[&](DWORD* n, OVERLAPPED* op) { return WriteFile(pipe,cursor,length,n,op); });
        if (!count) fail(ERROR_BROKEN_PIPE,"management pipe write");
        cursor += count;
        size -= count;
    }
}

// 先核对帧大小，再为正文分配内存并读完。
std::string receive_frame(HANDLE pipe, std::size_t limit, Deadline deadline) {
    std::uint32_t size = 0;
    read_all(pipe,&size,sizeof(size),deadline);
    if (size>limit) fail(ERROR_INVALID_DATA,"management frame length");
    std::string value(size,'\0');
    read_all(pipe,value.data(),value.size(),deadline);
    return value;
}

// 在协议预算内以长度前缀发送一个完整帧。
void send_frame(HANDLE pipe, const std::string& value, std::size_t limit, Deadline deadline) {
    if (value.size()>limit) fail(ERROR_INVALID_DATA,"management frame length");
    const auto size = static_cast<std::uint32_t>(value.size());
    write_all(pipe,&size,sizeof(size),deadline);
    write_all(pipe,value.data(),value.size(),deadline);
}
}

struct LocalPipe::State { Handle pipe; };
struct ProfileLease::State { Handle file; };

// 用独占文件句柄协调所有会创建本体的产品入口和目录别名。
// 步骤：准备数据目录 → 请求独占写入资格 → 区分已被占用和其他系统错误。
ProfileLease::ProfileLease(const std::filesystem::path& profile, app::SoftwareLog* log) : state_(std::make_unique<State>()) {
    std::error_code ec;
    std::filesystem::create_directories(profile,ec);
    if (ec) {
        if (log) log->record(app::SoftwareEvent::profile_claim,app::SoftwareResult::failed,{.system_error=ec.value()});
        throw std::filesystem::filesystem_error("management profile",profile,ec);
    }
    const auto path = profile/L".dafeiyu-runtime.lock";
    const auto file = CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (file==INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        const bool busy = code==ERROR_SHARING_VIOLATION || code==ERROR_LOCK_VIOLATION;
        if (log) log->record(app::SoftwareEvent::profile_claim,busy ? app::SoftwareResult::busy : app::SoftwareResult::failed,{.system_error=code});
        if (busy)
            throw std::runtime_error("DaFeiYu: this data directory is already in use by a host or terminal.");
        fail(code,"management profile lease");
    }
    state_->file.reset(file);
    if (log) log->record(app::SoftwareEvent::profile_claim,app::SoftwareResult::ok);
}

// 关闭独占句柄，之后其他进程可直接取得同一目录。
ProfileLease::~ProfileLease() = default;

// 以当前用户专属访问表创建唯一实例，在同一目录重复启动时由系统拒绝。
// 步骤：定位或建立用户目录 → 构造当前用户的访问表 → 独占创建本地命名管道。
LocalPipe::LocalPipe(const std::filesystem::path& profile) : state_(std::make_unique<State>()) {
    std::filesystem::create_directories(profile);
    const auto sid = user_sid(GetCurrentProcess());
    const auto name = pipe_name(profile,sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto rules = L"D:P(A;;GA;;;"+sid+L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(rules.c_str(),SDDL_REVISION_1,&descriptor,nullptr))
        fail(GetLastError(),"management pipe permissions");
    std::unique_ptr<void,decltype(&LocalFree)> release(descriptor,&LocalFree);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
    state_->pipe = own(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,64*1024,64*1024,100,&attributes),"management profile owner");
}

// 销毁最后一个服务端句柄，释放此用户目录的端点。
LocalPipe::~LocalPipe() = default;

// 在单个连接中处理一项请求，等客户读完回复后再断开并允许下次连接。
// 步骤：有界等候连接 → 读帧并处理 → 等待读取确认 → 断开并保留端点所有权。
bool LocalPipe::serve_one(std::chrono::milliseconds wait, const Handler& handle) {
    HANDLE pipe = state_->pipe.get();
    struct Disconnect {
        HANDLE pipe;
        // 清理当前连接，未完成客户端不影响下一次接入。
        ~Disconnect() { DisconnectNamedPipe(pipe); }
    } disconnect{pipe};
    try {
        transfer(pipe,Clock::now()+wait,[&](DWORD*, OVERLAPPED* op) {
            if (ConnectNamedPipe(pipe,op)) return TRUE;
            return GetLastError()==ERROR_PIPE_CONNECTED ? TRUE : FALSE;
        });
        const auto end = Clock::now()+std::chrono::seconds(5);
        const auto input = receive_frame(pipe,request_limit,end);
        const auto reply = handle(input);
        send_frame(pipe,reply,response_limit,end);
        char ack = 0;
        read_all(pipe,&ack,1,end);
        return ack == 1;
    } catch (const std::system_error& error) {
        const auto code = error.code().value();
        if (code==ERROR_SEM_TIMEOUT || code==ERROR_BROKEN_PIPE || code==ERROR_NO_DATA || code==ERROR_PIPE_NOT_CONNECTED ||
            code==ERROR_INVALID_DATA) return false;
        throw;
    }
}

// 连接同一用户的管道服务，核对实际服务进程后再发送请求。
// 步骤：定位目录并等待可用实例 → 核对服务进程用户 → 交换有界帧并确认已读。
std::string local_request(const std::filesystem::path& profile, const std::string& request, std::chrono::milliseconds timeout, const PeerReply& before_ack) {
    const auto end = Clock::now()+timeout;
    const auto sid = user_sid(GetCurrentProcess());
    const auto name = pipe_name(profile,sid);
    Handle pipe;
    for (;;) {
        HANDLE raw = CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        if (raw != INVALID_HANDLE_VALUE) { pipe.reset(raw); break; }
        const auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY) fail(error,"management connect");
        const auto delay = remaining(end);
        if (!delay) fail(ERROR_SEM_TIMEOUT,"management connect timeout");
        if (!WaitNamedPipeW(name.c_str(),delay)) fail(GetLastError(),"management connect wait");
    }

    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe.get(),&pid)) fail(GetLastError(),"management server identity");
    const DWORD access = PROCESS_QUERY_LIMITED_INFORMATION | (before_ack ? SYNCHRONIZE : 0);
    auto process = own(OpenProcess(access,FALSE,pid),"management server process");
    if (user_sid(process.get()) != sid) fail(ERROR_ACCESS_DENIED,"management server user");
    send_frame(pipe.get(),request,request_limit,end);
    auto reply = receive_frame(pipe.get(),response_limit,end);
    if (before_ack) before_ack(process.get(),pid,reply);
    const char ack = 1;
    write_all(pipe.get(),&ack,1,end);
    return reply;
}
}
