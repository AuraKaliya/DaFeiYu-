#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dfy::agent {
// 宿主绑定事实提供方；分别提供声明、原采样及当前有效的关注，省略关注时沿用声明的默认值。
struct DecisionStatusSource {
    std::string owner, definitions, samples;
    std::string attention{};
};
namespace status_detail { struct FrozenRecord; }
// 只有实际提交过的快照能签发回执，调用者无须重解析尾注即可关联原模型响应。
using PublishedDecisionStatus = std::shared_ptr<const status_detail::FrozenRecord>;
// 可移动的当步候选；tail 最多 8 个显式项、16 KiB，full 为含隐式项的完整快照对象。
struct FrozenDecisionStatus {
    std::string tail, full;
    // 允许宿主在需要本步冻结时再赋值。
    FrozenDecisionStatus() = default;
    // 唯一候选随请求移动，模型提交后移交不会复制正文。
    FrozenDecisionStatus(FrozenDecisionStatus&&) noexcept = default;
    // 更换待提交候选，自动释放被丢弃的预留。
    FrozenDecisionStatus& operator=(FrozenDecisionStatus&&) noexcept = default;
  private:
    friend class DecisionStatusStore;
    std::shared_ptr<const status_detail::FrozenRecord> record_;
};
// 一个 Session 的一个有效绑定持有独立存储；最近 32 个已发布版本支持并发只读查询。
class DecisionStatusStore {
  public:
    // 建立独立绑定身份，预留版本索引和注册槽。
    DecisionStatusStore();
    // 释放本绑定的注册原件及保留版本。
    ~DecisionStatusStore();
    // 按原采样冻结本步；rebuild 重发显式声明，可选 task 为最多 128 个 UTF-8 字节的宿主任务引用。
    FrozenDecisionStatus prepare(std::uint64_t session, std::uint64_t epoch, std::uint64_t turn,
        std::uint64_t step, std::int64_t utc_ms, const std::vector<DecisionStatusSource>& sources,
        bool rebuild = false, std::string_view task = {}) const;
    // 请求成功提交后移动候选入索引；不分配内存，空候选及其他存储的候选无效。
    PublishedDecisionStatus publish(FrozenDecisionStatus&& frozen) noexcept;
    // 结构有效的模型响应只为该逻辑决策实际展示的计次项扣一次，跨绑定及重复回调无效。
    void accept_response(const PublishedDecisionStatus& submitted) noexcept;
    // 按调用方的作用域读取冻结原版；含 state/status 的回包最多 64 KiB，窗口外或域不符返回原因。
    std::string read(std::uint64_t session, std::uint64_t epoch, std::string_view snapshot) const;
  private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
};
}
