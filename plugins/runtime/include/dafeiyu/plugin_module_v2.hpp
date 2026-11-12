#pragma once
#include <dafeiyu/plugin_manifest.hpp>
#include <dafeiyu/code_module.hpp>
#include <dafeiyu/plugin_abi_v2.h>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace dfy::plugins {
class PluginModuleV2;
template<class T> class Capability;

// 持有一份能力表副本和活动保护；函数地址只在本保护有效期间调用。
template<class T>
class CapabilityCall {
  public:
    // 禁止复制活动保护。
    CapabilityCall(const CapabilityCall&) = delete;
    // 禁止赋值覆盖仍在使用的活动调用。
    CapabilityCall& operator=(const CapabilityCall&) = delete;
    // 转交活动保护并使来源的表访问失效。
    CapabilityCall(CapabilityCall&& other) noexcept
        : api_(std::exchange(other.api_, {})), call_(std::move(other.call_)) {}
    // 返回本次活动保护覆盖的只读能力表，移动后拒绝访问。
    const T& api() const {
        if (!api_.header.struct_size) throw std::logic_error("plugin v2: empty capability call");
        return api_;
    }
  private:
    friend class Capability<T>;
    // 接收表副本和独立活动保护，不依赖能力持有者继续存活。
    CapabilityCall(T api, platform::CodeCall call) : api_(api), call_(std::move(call)) {}
    T api_;
    platform::CodeCall call_;
};

// 独立的静态能力持有；不直接暴露未受活动保护的函数地址。
template<class T>
class Capability {
  public:
    // 禁止隐式复制代码持有，另份持有通过 retain 取得并计费。
    Capability(const Capability&) = delete;
    // 禁止复制赋值覆盖已有代码持有。
    Capability& operator=(const Capability&) = delete;
    // 默认移动转交一次代码持有，来源后续 enter 会拒绝。
    Capability(Capability&&) = default;
    // 默认移动赋值归还旧代码持有并接收新持有。
    Capability& operator=(Capability&&) = default;
    // 显式取得独立能力持有，仍受原模块代码凭证容量限制。
    Capability retain() const { return Capability(api_, code_.retain()); }
    // 在进入能力函数前取得独立活动保护，覆盖对应调用及必要收尾。
    CapabilityCall<T> enter() const { return CapabilityCall<T>(api_, code_.enter()); }
  private:
    friend class PluginModuleV2;
    // 接收已校验的能力副本及持续代码凭证。
    Capability(T api, platform::CodeLease code) : api_(api), code_(std::move(code)) {}
    T api_;
    platform::CodeLease code_;
};

// 独立的 v2 模块控制器；复用代码寿命保护，不隐式改变 v1 或创建实例。
class PluginModuleV2 {
  public:
    // 复核清单并在活动保护内查询精确 v2 引导表，失败回收本次模块引用。
    PluginModuleV2(const Manifest& manifest, std::size_t lease_capacity, std::size_t call_capacity = 1);
    // 默认释放控制方持有，独立代码凭证仍保持模块寿命。
    ~PluginModuleV2() = default;
    // 禁止复制模块控制责任。
    PluginModuleV2(const PluginModuleV2&) = delete;
    // 禁止赋值或隐式移动，保持控制器及其借用关系稳定。
    PluginModuleV2& operator=(const PluginModuleV2&) = delete;
    // 查询本控制器是否仍持有已校验的引导表。
    bool open() const noexcept { return api_.query_interface != nullptr; }
    // 按有效 UTF-8 名称和精确版本查询静态能力，未知能力返回空值；必需函数由具体能力调用方核对。
    // 步骤：限定 C 表布局 → 受控查询并校验前缀 → 复制表并转交代码持有。
    template<class T>
    std::optional<Capability<T>> query(std::string_view name, std::uint32_t version) {
        static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>);
        static_assert(std::is_same_v<decltype(T::header), dfy_v2_api_header>);
        static_assert(offsetof(T, header) == 0 && sizeof(T) >= sizeof(dfy_v2_api_header) && sizeof(T) <= UINT32_MAX);
        auto result = query_table(name, version, static_cast<std::uint32_t>(sizeof(T)));
        if (!result) return std::nullopt;
        T copy{};
        std::memcpy(&copy, result->table, sizeof(T));
        return Capability<T>(copy, std::move(result->code));
    }
    // 尝试关闭本次模块引用，忙碌或仍有代码凭证时保持接口，释放失败可重试。
    platform::ModuleClose close();
  private:
    struct BoundTable { const void* table; platform::CodeLease code; };
    // 在活动保护下查询静态能力，精确校验大小及版本，返回绑定代码持有的借用。
    std::optional<BoundTable> query_table(std::string_view name, std::uint32_t version, std::uint32_t size);
    platform::CodeModule module_;
    dfy_v2_plugin_api api_{};
};
}
