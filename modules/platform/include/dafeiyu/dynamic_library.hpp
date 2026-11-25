#pragma once
#include <filesystem>
#include <string>

namespace dfy::platform {
// 单线程持有一次 Windows 模块引用；调用方保证所有调用及借用地址在关闭前结束。
class DynamicLibrary {
  public:
    using Symbol = void (*)();

    // 从绝对 DLL 路径受控加载，使用规范化路径并限制依赖搜索目录。
    explicit DynamicLibrary(const std::filesystem::path& file);
    // 作用域结束时尽力释放本次引用，不抛出异常；需要报告释放错误时先调用 close。
    ~DynamicLibrary();
    // 禁止复制或隐式移动，保持模块引用的释放责任唯一。
    DynamicLibrary(const DynamicLibrary&) = delete;
    // 禁止赋值，避免覆盖仍被调用方借用的模块。
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    // 查找导出函数地址，调用方按真实签名转换；地址仅在模块仍被持有时有效。
    Symbol symbol(const std::string& name) const;
    // 显式释放本次引用，重复关闭无操作；失败抛错且保留句柄供重试。
    void close();

  private:
    void* handle_ = nullptr;
};
}
