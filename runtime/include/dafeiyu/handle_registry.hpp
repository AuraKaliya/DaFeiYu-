#pragma once
#include <dafeiyu/runtime_ids.hpp>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dfy::runtime {
struct Handle {
    DomainId domain = 0;
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    // 判断完整三元组是否为空，不把部分为零的错误标识当作空值。
    bool empty() const noexcept { return !domain && !id && !generation; }
    // 按完整三元组比较引用身份，不代表对象身份。
    bool operator==(const Handle&) const = default;
};
enum class ReferenceFailure { invalid_handle, not_shareable, exhausted };
class ReferenceError : public std::runtime_error {
  public:
    // 保存可供后续 C ABI 适配分类的引用错误，不借用对象或 DLL 内存。
    explicit ReferenceError(ReferenceFailure reason) : std::runtime_error("runtime: reference failure"), reason_(reason) {}
    // 返回引用失败的确定类别。
    ReferenceFailure reason() const noexcept { return reason_; }
  private:
    ReferenceFailure reason_;
};
namespace detail {
// 推进引用代次，零为永久停用；最大值释放后不回绕。
inline constexpr std::uint64_t following_generation(std::uint64_t current) noexcept {
    return !current || current == std::numeric_limits<std::uint64_t>::max() ? 0 : current + 1;
}
}

// 同一宿主线程使用的有界类型化引用表；对象析构不得重入本表。
// 每表独立签发域，借用地址仅在对应引用有效期间使用；不承担异步工作的停止责任。
template<class T>
class HandleRegistry {
  public:
    // 分配独立域和固定容量槽位，失败不返回半构造表，已消耗域也不复用。
    // 步骤：签发独立域 → 分配槽位 → 拒绝空容量。
    explicit HandleRegistry(std::size_t capacity) : domain_(next_domain()), slots_(capacity) {
        if (!capacity) throw std::invalid_argument("runtime: positive reference capacity required");
    }
    // 释放本表持有的引用；未结束的宿主工作必须另有独立所有者。
    ~HandleRegistry() = default;
    // 禁止复制签发域和引用记录。
    HandleRegistry(const HandleRegistry&) = delete;
    // 禁止赋值或隐式移动，保持引用表归属稳定。
    HandleRegistry& operator=(const HandleRegistry&) = delete;

    // 登记非空对象并签发持有引用，满表或代次耗尽时不改变已有记录。
    // 步骤：拒绝空对象 → 查找可用槽位 → 保存对象与共享规则并签发引用。
    Handle insert(std::shared_ptr<T> object, bool shareable = true) {
        if (!object) throw std::invalid_argument("runtime: nonempty object required");
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            auto& slot = slots_[i];
            if (slot.object || !slot.generation) continue;
            slot.object = std::move(object);
            slot.shareable = shareable;
            ++size_;
            return {domain_, i + 1, slot.generation};
        }
        throw ReferenceError(ReferenceFailure::exhausted);
    }
    // 保留同一对象的独立引用，原始标识的数值复制不会执行此操作。
    // 步骤：验证来源 → 核对共享规则 → 复用登记流程签发独立引用。
    Handle retain(Handle source) {
        const auto& slot = checked(source);
        if (!slot.shareable) throw ReferenceError(ReferenceFailure::not_shareable);
        return insert(slot.object);
    }
    // 释放一份持有引用，空值无操作，旧别名立即失效且不影响其他引用。
    // 步骤：处理空值并验证引用 → 移出对象 → 推进代次并更新计量 → 释放本次持有。
    void release(Handle owned) {
        if (owned.empty()) return;
        (void)checked(owned);
        auto& slot = slots_[static_cast<std::size_t>(owned.id - 1)];
        auto discarded = std::move(slot.object);
        slot.generation = detail::following_generation(slot.generation);
        --size_;
    }
    // 借用有效引用对应的只读对象，不增加持有责任或允许借用者释放来源引用。
    const T& get(Handle source) const { return *checked(source).object; }
    // 返回当前表的签发域，表销毁后该域也不会再次分配。
    DomainId domain() const noexcept { return domain_; }
    // 返回当前有效持有引用数，不代表对象数量或异步任务数量。
    std::size_t size() const noexcept { return size_; }
    // 在外层停止使用后释放全部引用并推进代次，不执行异步取消或等待。
    // 步骤：移出每份持有 → 使旧标识失效并更新计量 → 释放对应对象。
    void clear() noexcept {
        for (auto& slot : slots_) {
            if (!slot.object) continue;
            auto discarded = std::move(slot.object);
            slot.generation = detail::following_generation(slot.generation);
            --size_;
        }
    }
    // 返回仍可签发的空槽数，代次耗尽后永久停用的槽位不计入。
    std::size_t available() const noexcept {
        std::size_t result = 0;
        for (const auto& slot : slots_) if (!slot.object && slot.generation) ++result;
        return result;
    }

  private:
    struct Slot {
        std::uint64_t generation = 1;
        std::shared_ptr<T> object;
        bool shareable = true;
    };
    // 验证签发域、槽位、代次与有效引用后返回记录，不解引用调用方地址。
    // 步骤：核对域与编号 → 核对记录及代次 → 返回有效槽位。
    const Slot& checked(Handle source) const {
        if (source.domain != domain_ || !source.id || source.id > slots_.size()) {
            throw ReferenceError(ReferenceFailure::invalid_handle);
        }
        const auto& slot = slots_[static_cast<std::size_t>(source.id - 1)];
        if (!slot.object || !source.generation || source.generation != slot.generation) {
            throw ReferenceError(ReferenceFailure::invalid_handle);
        }
        return slot;
    }
    DomainId domain_;
    std::vector<Slot> slots_;
    std::size_t size_ = 0;
};
}
