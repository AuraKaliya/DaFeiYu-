#pragma once
#include <dafeiyu/agent_abi_v2.h>
#include <deque>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>

namespace dfy::mc {
// 仅在指定玩家的当前配对会话中持有事实镜像，派发仍走唯一的原 Agent。
class PartnerFacts {
  public:
    // 绑定可选事实服务，旧宿主未提供时沿用普通子会话。
    void configure(const dfy_v2_agent_facts_api* service, std::string player, const dfy_v2_agent_ordered_facts_api* ordered = nullptr);
    // 为试点玩家协商有序回执或旧事实服务，未配置玩家时保持普通入口。
    std::uint32_t connect(dfy_v2_query_interface_fn host, const std::string& player);
    // 查询当前玩家是否为显式选定的试点。
    bool enabled(std::string_view player) const noexcept { return service_ && player == player_; }
    // 查询当前工具是否属于已经提交事实基线的原会话。
    bool bound(std::uint64_t session) const noexcept { return service_ && session && session == session_; }
    // 查询宿主是否提供原账本读取能力。
    bool can_read_receipts() const noexcept { return ordered_ && ordered_->read_receipt; }
    // 在游戏派发前为原工作登记宿主责任，失败不消费新的接纳序号。
    std::uint32_t reserve(std::string_view work, std::string_view skill, std::uint64_t order);
    // 只清空本绑定的投影和原工作记录，下个绑定从完整基线开始。
    void clear() noexcept;
    // 分出旧绑定的原工作结算状态，供离场后只读核验的工作共同持有。
    std::shared_ptr<PartnerFacts> retire_settlements();
    // 发送基线或同作用域补丁，断序时按原号发送一次完整快照。
    std::uint32_t submit(std::uint64_t session, std::string_view scope, const dfy_v2_agent_input& run,
        const nlohmann::json& scene, bool isolate, std::uint64_t& request);
    // 把已核实的动作接纳及原工作结果写入同一绑定，不触发额外游戏动作。
    std::uint32_t report(std::uint64_t session, std::string_view category, const nlohmann::json& payload);
    // 识别已在本绑定登记退出未知、仍需用原桥接工作查证的动作。
    bool needs_exit(std::uint64_t session, const std::string& work) const noexcept;
    // 查询原交物是否已有可追踪的投物证据，仍需核清去向。
    bool needs_pickup(std::uint64_t session, const std::string& work) const noexcept;
    // 按当前绑定的代次和回执号只读宿主原件，归档记录沿用宿主返回的指纹。
    nlohmann::json read_work_receipt(std::uint64_t session, const nlohmann::json& args) const;
    // 在原绑定内结算已通过协议核验的同号拾取样本。
    std::uint32_t settle_pickup(std::uint64_t session, const std::string& work, const nlohmann::json& sample);
  private:
    // 向宿主提交本绑定原工作的一个确定版本，重复提交沿用原回执。
    std::uint32_t put(std::string_view work, std::uint64_t revision, std::uint32_t phase, const nlohmann::json& body,
        bool final = false, std::uint64_t sequence = 0) const;
    // 依据原投物工作登记退出，所提交物品的去向核清后登记实际收取者。
    std::uint32_t report_handoff(const nlohmann::json& handoff);
    // 已无需继续复查的本地阶段只保留最近一窗，原宿主继续校验过期重传。
    void finish_late(const std::string& work);
    struct LateReceipt {
        bool exit_was_unknown = false, exited = false, effect_after_exit = false;
        bool handoff = false, pending_pickup = false, pickup_settled = false;
        nlohmann::json chest_basis;
        bool chest_settled = false;
    };
    std::map<std::string,LateReceipt> uncertain_;
    std::deque<std::string> finished_late_;
    const dfy_v2_agent_facts_api* service_ = nullptr;
    const dfy_v2_agent_ordered_facts_api* ordered_ = nullptr;
    std::string player_, scope_;
    nlohmann::json previous_;
    std::uint64_t sequence_ = 0, session_ = 0, admission_ = 0;
    bool isolated_ = false;
};
}
