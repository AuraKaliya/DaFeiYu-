#include "resident_watch.hpp"
#include <windows.h>
#include <string>
#include <system_error>

namespace dfy::management {
namespace {
using Handle = std::unique_ptr<void,decltype(&CloseHandle)>;

// 从本体签发的数字身份派生本次桌面会话内的固定事件名。
std::wstring signal_name(std::string_view instance) {
    if (instance.empty() || instance.size()>40 || instance.find_first_not_of("0123456789-")!=instance.npos)
        throw std::invalid_argument("management: invalid resident identity");
    return L"Local\\DaFeiYu.Management.Stop."+std::wstring(instance.begin(),instance.end());
}

// 保留原生对象操作的实际失败原因。
void fail(DWORD code) { throw std::system_error(static_cast<int>(code),std::system_category(),"management lifecycle"); }
}

struct StopSignal::State { Handle signal{nullptr,&CloseHandle}; bool accepted = false; };
struct ResidentWatch::State { Handle process{nullptr,&CloseHandle}, signal{nullptr,&CloseHandle}; std::uint32_t pid; };

// 仅创建本次实例的未置位事件，已有对象不能代表本次退出。
StopSignal::StopSignal(std::string_view instance) : state_(std::make_unique<State>()) {
    const auto name = signal_name(instance);
    SetLastError(ERROR_SUCCESS);
    const auto event = CreateEventW(nullptr,TRUE,FALSE,name.c_str());
    const auto error = GetLastError();
    state_->signal.reset(event);
    if (!state_->signal || error==ERROR_ALREADY_EXISTS) fail(error);
}

// 释放本体的事件引用，窗口自己的等待句柄可跨越本体实退。
StopSignal::~StopSignal() = default;

// 确认事件持续置位后接纳首次退出，重复请求复用原意图。
bool StopSignal::accept() {
    if (state_->accepted) return false;
    if (!SetEvent(state_->signal.get())) fail(GetLastError());
    state_->accepted = true;
    return true;
}

// 复制管道已核对的原进程对象，支持生命周期协议的实例还须取得其意图事件。
// 步骤：复制查询与同步权限 → 检查进程身份 → 打开只可等待的实例事件。
ResidentWatch::ResidentWatch(void* process, std::uint32_t pid, std::string_view instance, bool has_signal)
    : state_(std::make_unique<State>()) {
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(),process,GetCurrentProcess(),&copy,
        SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,0)) fail(GetLastError());
    state_->process.reset(copy);
    if (!pid || GetProcessId(copy)!=pid) fail(ERROR_INVALID_DATA);
    state_->pid = pid;
    if (has_signal) {
        const auto name = signal_name(instance);
        state_->signal.reset(OpenEventW(SYNCHRONIZE,FALSE,name.c_str()));
        if (!state_->signal) fail(GetLastError());
    }
}

// 归还观察者持有的两个句柄，不向原实例发出任何操作。
ResidentWatch::~ResidentWatch() = default;

// 先核实原进程是否退出，再读取意图，覆盖在两次采样之间完成的正常退出。
// 步骤：零等待检查原进程 → 按实际退出读取返回码 → 核对同实例的持续置位事件。
ResidentObservation ResidentWatch::observe() const noexcept {
    ResidentObservation result{.pid=state_->pid};
    const auto wait = WaitForSingleObject(state_->process.get(),0);
    if (wait==WAIT_OBJECT_0) {
        DWORD code = 0;
        if (GetExitCodeProcess(state_->process.get(),&code)) result.exit_code = code;
        else result.error = GetLastError();
    } else if (wait==WAIT_FAILED) result.error = GetLastError();
    if (state_->signal) {
        const auto intent = WaitForSingleObject(state_->signal.get(),0);
        if (intent==WAIT_OBJECT_0) result.requested = true;
        else if (intent==WAIT_FAILED && !result.error) result.error = GetLastError();
    }
    return result;
}
}
