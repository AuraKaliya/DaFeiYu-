#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dfy::agent {
// 宿主绑定事实提供方；两个 JSON 数组分别声明状态项和提供当步原采样。
struct DecisionStatusSource {
    std::string owner, definitions, samples;
};
namespace status_detail { struct FrozenRecord; }
// 可移动的当步候选；tail 和 full 分别为送模投影与完整快照 JSON。
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
    // 按当前采样和 UTC 毫秒准备本步；rebuild 在同绑定历史重建后重发实际展示项的声明。
    FrozenDecisionStatus prepare(std::uint64_t session, std::uint64_t epoch, std::uint64_t turn,
        std::uint64_t step, std::int64_t utc_ms, const std::vector<DecisionStatusSource>& sources, bool rebuild = false) const;
    // 请求成功提交后移动候选入索引；不分配内存，空候选及其他存储的候选无效。
    void publish(FrozenDecisionStatus&& frozen) noexcept;
    // 按调用方的作用域读取冻结原版；裁剪、未知和绑定不符分别说明原因。
    std::string read(std::uint64_t session, std::uint64_t epoch, std::string_view snapshot) const;
  private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
};
}
