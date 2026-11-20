#pragma once
#include "partner_feed.hpp"
#include <dafeiyu/agent_abi_v2.h>
#include <dafeiyu/companion_abi_v2.h>

namespace dfy::mc {
struct PartnerWork { std::uint64_t invocation = 0; std::string_view skill; };
// Minecraft 插件拥有事件处理、角色规则与游戏技能，模型工具循环借用宿主子会话。
class PartnerLoop {
  public:
    // 协商宿主子会话能力并启用原玩家入口。
    uint32_t listen(dfy_v2_query_interface_fn host, const BridgeConfig& config);
    // 停止读取并请求取消原在途回合。
    void stop() noexcept;
    // 在插件槽被回收时归还其私有的只读连接和事件。
    void reset() noexcept;
    // 非等待推进配对读取与原子会话，一个步骤最多接纳一个回合。
    void advance(const BridgeConfig& config, std::uint64_t instance, PartnerWork work);
    // 新事实或世界变化时拦住旧会话尚未开始的步骤，保留已接纳动作的收尾。
    bool permits(std::uint64_t session) const noexcept;
    // 查询已绑定玩家和实际回合水位。
    dfy_v2_partner_status status() const noexcept;
  private:
    // 当前回合实际退出后可发布原结算，或请求取消并继续等待。
    bool settle(bool cancel);
    // 用原游戏游标、观察及四项技能构造一轮，接纳后消费原件。
    void submit();
    // 结束旧游戏绑定对应的 Agent 上下文。
    void retire();
    // 单独判断新事实是否应中止原生在途动作，过期判断不得中止替代动作。
    void review(PartnerWork work, bool closing);

    PartnerFeed feed_;
    const dfy_v2_agent_api* agent_ = nullptr;
    std::string session_world_;
    std::uint64_t session_ = 0, request_ = 0, order_ = 0, settled_ = 0;
    struct Review {
        std::uint64_t session = 0, request = 0, origin = 0, work = 0, cursor = 0;
    } review_;
    nlohmann::json events_, previous_;
    std::uint64_t reviewed_ = 0;
    bool stopped_work_ = false;
    uint32_t phase_ = DFY_V2_PARTNER_CLOSED;
};
}
