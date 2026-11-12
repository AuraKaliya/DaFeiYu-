#pragma once
#include <dafeiyu/dynamic_library.hpp>
#include <cstddef>
#include <memory>

namespace dfy::platform {
enum class ModuleClose { closed, in_use, busy };
class CodeLease;
class CodeCall;

// 同一宿主线程内管理模块及有界代码持有，尚不校验插件 ABI 或处理实例生命周期。
class CodeModule {
  public:
    // 先校验正凭证及调用容量，再加载真实 DLL；缺省允许一个活动调用。
    CodeModule(const std::filesystem::path& file, std::size_t lease_capacity, std::size_t call_capacity = 1);
    // 释放控制方持有；仍存活的代码凭证继续保持模块，最后持有者尽力释放引用。
    ~CodeModule() = default;
    // 禁止复制模块控制责任。
    CodeModule(const CodeModule&) = delete;
    // 禁止赋值或隐式移动控制对象。
    CodeModule& operator=(const CodeModule&) = delete;
    // 取得一份独立代码凭证，关闭后拒绝；容量不足抛出 length_error。
    CodeLease lease();
    // 活动调用时返回 busy，否则有凭证时返回 in_use；释放失败保留可重试状态。
    ModuleClose close();
    // 返回代码凭证数量，不代表当前原生调用数。
    std::size_t leases() const noexcept { return storage_->leases; }
    // 返回活动保护数量，覆盖入口准备、原生执行和必要收尾。
    std::size_t active_calls() const noexcept { return storage_->active_calls; }
  private:
    friend class CodeLease;
    friend class CodeCall;
    struct Storage {
        // 加载并持有一次平台模块引用，记录代码凭证及活动保护上限。
        Storage(const std::filesystem::path& file, std::size_t limit, std::size_t calls)
            : library(file), capacity(limit), call_capacity(calls) {}
        DynamicLibrary library;
        std::size_t capacity, call_capacity, leases = 0, active_calls = 0;
        bool closed = false;
    };
    std::shared_ptr<Storage> storage_;
};

// 独立代码寿命凭证，须覆盖调用、排队恢复及依赖 DLL 的销毁代码；仅在宿主内使用。
class CodeLease {
  public:
    // 创建不持有模块的空凭证。
    CodeLease() = default;
    // 归还本次凭证，最后存活持有者由平台库尽力释放模块。
    ~CodeLease() { reset(); }
    // 禁止隐式复制，独立保留须显式调用 retain 并计费。
    CodeLease(const CodeLease&) = delete;
    // 禁止复制赋值覆盖代码持有责任。
    CodeLease& operator=(const CodeLease&) = delete;
    // 移动转交本次凭证，不增加计量。
    CodeLease(CodeLease&& other) noexcept : storage_(std::move(other.storage_)) {}
    // 归还旧凭证后接收新持有，自移动无操作。
    CodeLease& operator=(CodeLease&& other) noexcept;
    // 显式保留同一模块的独立凭证，受同一容量约束。
    CodeLease retain() const;
    // 借用导出地址，只在相应代码凭证有效期间使用；空凭证拒绝。
    DynamicLibrary::Symbol symbol(const std::string& name) const;
    // 在调用或依赖 DLL 的销毁入口前建立活动保护，必须保持到该入口返回宿主。
    CodeCall enter() const;
    // 归还本次代码凭证，空凭证重复释放无操作。
    void reset() noexcept;
    // 判断是否仍持有模块代码。
    explicit operator bool() const noexcept { return static_cast<bool>(storage_); }
  private:
    friend class CodeModule;
    // 校验模块及容量后增加一次代码持有，不接触插件业务状态。
    explicit CodeLease(std::shared_ptr<CodeModule::Storage> storage);
    std::shared_ptr<CodeModule::Storage> storage_;
};

// 独立保持活动调用及模块寿命；只在宿主线程移动，不能在原生调用返回前释放。
class CodeCall {
  public:
    // 创建不代表活动调用的空保护。
    CodeCall() = default;
    // 结束调用保护，最后持有者在原生入口已返回后尽力释放模块。
    ~CodeCall() { reset(); }
    // 禁止复制活动计量。
    CodeCall(const CodeCall&) = delete;
    // 禁止复制赋值活动保护。
    CodeCall& operator=(const CodeCall&) = delete;
    // 移动转交活动保护，来源随即为空。
    CodeCall(CodeCall&& other) noexcept : storage_(std::move(other.storage_)) {}
    // 结束旧保护后接收新活动持有，自移动无操作。
    CodeCall& operator=(CodeCall&& other) noexcept;
    // 在活动保护下借用导出地址，原代码凭证释放后该借用仍由本保护覆盖。
    DynamicLibrary::Symbol symbol(const std::string& name) const;
    // 在调用和必要收尾返回宿主后结束保护，空保护重复结束无操作。
    void reset() noexcept;
  private:
    friend class CodeLease;
    // 核对活动容量并独立保持模块，失败不增加活动计量。
    explicit CodeCall(std::shared_ptr<CodeModule::Storage> storage);
    std::shared_ptr<CodeModule::Storage> storage_;
};
}
