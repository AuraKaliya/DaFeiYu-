#include "host_settings_file.hpp"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <atomic>
#include <charconv>
#include <set>
#include <utility>
#include <vector>

namespace dfy::app {
namespace {
using Json = nlohmann::json;
constexpr DWORD max_file_size = 32*1024;
struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    // 归还本次读取或待发布副本的唯一文件句柄。
    ~File() { if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
struct Temporary {
    std::filesystem::path path;
    // 丢弃仅属于本次保存、尚未发布的临时文件。
    ~Temporary() { if (!path.empty()) DeleteFileW(path.c_str()); }
};

// 解码文件中无符号十进制修订，拒绝溢出和非规范表示。
std::uint64_t stored_revision(const Json& value) {
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(text.data(),text.data()+text.size(),number);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || number<2 || text.front()=='0')
        throw std::runtime_error("settings: invalid stored revision");
    return number;
}
}

// 读取独立主体配置和原提交回执，根字段与最后一条回执必须一致。
// 步骤：只读打开普通文件 → 限长解析 → 核对连续修订与字段 → 返回经核实的覆盖值。
std::deque<HostSettingWrite> read_host_settings_file(const std::filesystem::path& file) {
    File input{CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    if (input.handle==INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) return {};
        throw std::runtime_error("settings: cannot open user file");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(input.handle,&info) || (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) ||
        GetFileType(input.handle)!=FILE_TYPE_DISK || info.nFileSizeHigh || !info.nFileSizeLow || info.nFileSizeLow>max_file_size)
        throw std::runtime_error("settings: invalid user file");
    std::string bytes(info.nFileSizeLow,'\0');
    DWORD read = 0;
    if (!ReadFile(input.handle,bytes.data(),info.nFileSizeLow,&read,nullptr) || read!=info.nFileSizeLow)
        throw std::runtime_error("settings: incomplete user file");
    std::vector<std::set<std::string>> keys;
    // 限制配置结构深度并逐对象拒绝重复字段，防止歧义值进入启动配置。
    // 步骤：校验深度 → 登记每层对象的键 → 对象结束时归还键集合。
    const auto doc = Json::parse(bytes,[&](int depth, Json::parse_event_t event, Json& value) {
        if (depth>4) throw std::runtime_error("settings: invalid nesting");
        if (event==Json::parse_event_t::object_start) keys.emplace_back();
        else if (event==Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::runtime_error("settings: duplicate user field");
        else if (event==Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
    const auto& entries = doc.at("writes");
    if (!doc.at("version").is_number_integer() || doc.at("version")!=1 || doc.size()!=4 || !entries.is_array() || entries.empty() || entries.size()>settings_receipt_limit)
        throw std::runtime_error("settings: invalid user format");
    std::deque<HostSettingWrite> result;
    std::set<std::string> identities;
    for (const auto& item : entries) {
        auto submission = item.at("submission").get<std::string>();
        const auto revision = stored_revision(item.at("revision"));
        const auto& value = item.at("timeout_ms");
        if (item.size()!=3 || !valid_settings_submission(submission) || !identities.insert(submission).second ||
            (!result.empty() && revision-1!=result.back().revision) || !value.is_number_integer() || value<host_timeout_min || value>host_timeout_max)
            throw std::runtime_error("settings: invalid stored operation");
        result.push_back({std::move(submission),revision,value.get<int>()});
    }
    if ((result.size()<settings_receipt_limit && result.front().revision!=2) ||
        stored_revision(doc.at("revision"))!=result.back().revision || !doc.at("timeout_ms").is_number_integer() ||
        doc.at("timeout_ms")!=result.back().timeout_ms) throw std::runtime_error("settings: inconsistent user file");
    return result;
}

// 先刷新并关闭同目录的独占副本，再一次替换目标，任何失败均清理本次副本。
// 步骤：编码新修订和回执 → 独占创建并写完副本 → 刷新关闭 → 原子发布。
void write_host_settings_file(const std::filesystem::path& file, const std::deque<HostSettingWrite>& writes, bool replacing) {
    auto entries = Json::array();
    for (const auto& item : writes) entries.push_back({{"submission",item.submission},{"revision",std::to_string(item.revision)},{"timeout_ms",item.timeout_ms}});
    const auto& last = writes.back();
    const auto bytes = Json{{"version",1},{"revision",std::to_string(last.revision)},{"timeout_ms",last.timeout_ms},{"writes",std::move(entries)}}.dump(2);
    if (bytes.size()>max_file_size) throw std::length_error("settings: user file limit");
    static std::atomic_uint64_t sequence{};
    Temporary temporary;
    File output;
    for (unsigned i=0; i<16; ++i) {
        auto candidate = file;
        candidate += L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++sequence);
        output.handle=CreateFileW(candidate.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (output.handle!=INVALID_HANDLE_VALUE) { temporary.path=std::move(candidate); break; }
        const auto error=GetLastError();
        if (error!=ERROR_FILE_EXISTS && error!=ERROR_ALREADY_EXISTS) break;
    }
    if (output.handle==INVALID_HANDLE_VALUE) throw std::runtime_error("settings: cannot create user file");
    DWORD written = 0;
    if (!WriteFile(output.handle,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) || written!=bytes.size() || !FlushFileBuffers(output.handle))
        throw std::runtime_error("settings: cannot flush user file");
    const auto handle=std::exchange(output.handle,INVALID_HANDLE_VALUE);
    if (!CloseHandle(handle) || !MoveFileExW(temporary.path.c_str(),file.c_str(),MOVEFILE_WRITE_THROUGH|(replacing ? MOVEFILE_REPLACE_EXISTING : 0)))
        throw std::runtime_error("settings: cannot publish user file");
    temporary.path.clear();
}
}
