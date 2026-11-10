#include <dafeiyu/dynamic_library.hpp>
#include <windows.h>
#include <cwchar>
#include <stdexcept>

namespace dfy::platform {
namespace {
// 保留系统错误号并给出加载、查找或释放阶段的诊断。
// 步骤：保存 Windows 错误号 → 归类常见失败 → 抛出不依赖模块内存的错误。
[[noreturn]] void fail(const char* operation) {
    const auto code = GetLastError();
    const char* reason = "system error";
    if (code == ERROR_MOD_NOT_FOUND) {
        reason = "module or dependency not found";
    } else if (code == ERROR_BAD_EXE_FORMAT || code == ERROR_EXE_MACHINE_TYPE_MISMATCH) {
        reason = "wrong architecture or invalid image";
    } else if (code == ERROR_PROC_NOT_FOUND) {
        reason = "export not found";
    }
    throw std::runtime_error(std::string("library: ") + operation + ": " + reason + " (Win32 " + std::to_string(code) + ")");
}

class LoadErrorMode {
  public:
    // 临时关闭当前线程的加载错误对话框，让加载失败直接返回错误码。
    LoadErrorMode() {
        if (!SetThreadErrorMode(GetThreadErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_)) {
            fail("set thread error mode");
        }
    }
    // 正常或异常退出时恢复调用线程原来的错误处理模式。
    ~LoadErrorMode() { SetThreadErrorMode(previous_, nullptr); }
    // 禁止复制，保持线程模式恢复责任唯一。
    LoadErrorMode(const LoadErrorMode&) = delete;
    // 禁止赋值，避免覆盖尚未恢复的线程模式。
    LoadErrorMode& operator=(const LoadErrorMode&) = delete;

  private:
    DWORD previous_ = 0;
};
}

// 加载指定 DLL，只允许其所在目录和系统目录参与文件系统依赖搜索。
// 步骤：校验绝对 DLL 路径 → 规范化并确认普通文件 → 临时关闭错误对话框 → 加载并接管模块引用。
DynamicLibrary::DynamicLibrary(const std::filesystem::path& file) {
    if (!file.is_absolute() || file.native().find(L'\0') != std::wstring::npos ||
        _wcsicmp(file.extension().c_str(), L".dll") != 0) {
        throw std::invalid_argument("library: absolute .dll path without NUL required");
    }
    std::filesystem::path resolved;
    try {
        resolved = std::filesystem::canonical(file);
        if (!std::filesystem::is_regular_file(resolved) || _wcsicmp(resolved.extension().c_str(), L".dll") != 0) {
            throw std::runtime_error("library: DLL file unavailable");
        }
    } catch (const std::filesystem::filesystem_error&) {
        throw std::runtime_error("library: DLL file unavailable");
    }
    resolved.make_preferred();

    const LoadErrorMode error_mode;
    handle_ = LoadLibraryExW(resolved.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!handle_) {
        fail("load");
    }
}

// 在正常返回或异常退出时尽力释放对象持有的唯一模块引用。
DynamicLibrary::~DynamicLibrary() {
    if (handle_) {
        FreeLibrary(static_cast<HMODULE>(handle_));
    }
}

// 返回模块导出函数地址，查找失败不转移引用的归属。
// 步骤：检查模块状态和名称 → 查找导出 → 返回借用地址或报告错误。
DynamicLibrary::Symbol DynamicLibrary::symbol(const std::string& name) const {
    if (!handle_) {
        throw std::runtime_error("library: closed");
    }
    if (name.empty() || name.find('\0') != std::string::npos) {
        throw std::invalid_argument("library: nonempty export name without NUL required");
    }
    const auto address = GetProcAddress(static_cast<HMODULE>(handle_), name.c_str());
    if (!address) {
        fail("symbol lookup");
    }
    return reinterpret_cast<Symbol>(address);
}

// 释放本次取得的引用，只有系统确认成功后才清空句柄。
void DynamicLibrary::close() {
    if (handle_) {
        if (!FreeLibrary(static_cast<HMODULE>(handle_))) {
            fail("close");
        }
        handle_ = nullptr;
    }
}
}
