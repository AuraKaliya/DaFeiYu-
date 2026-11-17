#include "diagnostic_files.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <system_error>

namespace dfy::app {
namespace {
// 保存 Windows 错误码，不把诊断正文加入异常信息。
void fail(const char* operation) { throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),operation); }
// 拥有一个原生文件句柄，离开作用域始终归还，不复制关闭责任。
class File {
  public:
    // 接收已经成功打开的文件句柄。
    explicit File(HANDLE handle) : handle_(handle) {}
    // 归还仍持有的句柄，不抛出清理异常。
    ~File() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    // 禁止复制文件关闭责任。
    File(const File&) = delete;
    // 禁止赋值覆盖已有句柄。
    File& operator=(const File&) = delete;
    // 借用当前文件句柄。
    HANDLE get() const noexcept { return handle_; }
    // 发布文件前关闭写入句柄，失败明确报告。
    void close() {
        const auto handle = std::exchange(handle_,INVALID_HANDLE_VALUE);
        if (handle != INVALID_HANDLE_VALUE && !CloseHandle(handle)) fail("diagnostic close");
    }
  private:
    HANDLE handle_;
};
// 清理仅属于本次创建的临时文件；发布后原目标不进入清理路径。
struct Temporary {
    std::filesystem::path path;
    bool owned = false;
    // 删除未发布临时文件，不触碰任何已存在目标。
    ~Temporary() { if (owned) DeleteFileW(path.c_str()); }
};
// 固定明确的绝对文件路径，不接受目录名或 Windows 备用数据流。
std::filesystem::path file_path(const std::filesystem::path& path) {
    if (path.empty()) throw std::invalid_argument("diagnostic: explicit file path required");
    const auto absolute = std::filesystem::absolute(path).lexically_normal(); const auto name = absolute.filename().native();
    if (name.empty() || name == L"." || name == L".." || name.find(L':') != std::wstring::npos || absolute.native().size() > 32000)
        throw std::invalid_argument("diagnostic: invalid file path");
    return absolute;
}
}
// 先写完整副本再发布，不因目标同名或写入失败破坏已有文件。
// 步骤：编码并校验目标 → 独占创建临时文件 → 完整写入并刷新 → 关闭后无覆盖发布。
void write_diagnostic_file(const std::filesystem::path& destination, const agent::DiagnosticSnapshot& value) {
    const auto text = agent::encode_diagnostic(value); const auto target = file_path(destination);
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) throw std::invalid_argument("diagnostic: destination already exists");
    Temporary temporary; HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        temporary.path = target; temporary.path += L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(runtime::next_domain());
        handle = CreateFileW(temporary.path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (handle != INVALID_HANDLE_VALUE) { temporary.owned = true; break; }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) fail("diagnostic create");
    }
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("diagnostic: temporary name capacity exhausted");
    File file(handle); std::size_t offset = 0;
    while (offset < text.size()) {
        const auto size = static_cast<DWORD>(std::min<std::size_t>(text.size()-offset,1024*1024)); DWORD written = 0;
        if (!WriteFile(file.get(),text.data()+offset,size,&written,nullptr)) fail("diagnostic write");
        if (!written) throw std::runtime_error("diagnostic: write made no progress");
        offset += written;
    }
    if (!FlushFileBuffers(file.get())) fail("diagnostic flush");
    file.close();
    if (!MoveFileExW(temporary.path.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)) fail("diagnostic publish");
    temporary.owned = false;
}
// 文件入口只返回值摘要，内容中的任何标识均不能恢复原执行对象。
// 步骤：打开明确文件 → 在分配前检查总大小 → 读取完整文本 → 严格解析及回顾。
agent::DiagnosticSnapshot read_diagnostic_file(const std::filesystem::path& source) {
    const auto path = file_path(source); const auto handle = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (handle == INVALID_HANDLE_VALUE) fail("diagnostic open");
    File file(handle); LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(),&size)) fail("diagnostic size");
    if (size.QuadPart < 0 || size.QuadPart > 8*1024*1024) throw std::length_error("diagnostic: file exceeds 8 MiB");
    std::string text(static_cast<std::size_t>(size.QuadPart),'\0'); std::size_t offset = 0;
    while (offset < text.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(text.size()-offset,1024*1024)); DWORD read = 0;
        if (!ReadFile(file.get(),text.data()+offset,chunk,&read,nullptr)) fail("diagnostic read");
        if (!read) throw std::runtime_error("diagnostic: truncated file"); offset += read;
    }
    return agent::decode_diagnostic(text);
}
}
