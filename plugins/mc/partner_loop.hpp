#pragma once
#include "partner_feed.hpp"
#include <dafeiyu/agent_abi_v2.h>
#include <dafeiyu/companion_abi_v2.h>

namespace dfy::mc {
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
    void advance(const BridgeConfig& config, std::uint64_t instance);
    // 查询已绑定玩家和实际回合水位。
    dfy_v2_partner_status status() const noexcept;
  private:
    // 当前回合实际退出后可发布原结算，或请求取消并继续等待。
    bool settle(bool cancel);
    // 用原游戏游标、观察及四项技能构造一轮，接纳后消费原件。
    void submit();
    // 结束旧游戏绑定对应的 Agent 上下文。
    void retire();

    PartnerFeed feed_;
    const dfy_v2_agent_api* agent_ = nullptr;
    std::string session_world_;
    std::uint64_t session_ = 0, request_ = 0, order_ = 0, settled_ = 0;
    uint32_t phase_ = DFY_V2_PARTNER_CLOSED;
};
}
