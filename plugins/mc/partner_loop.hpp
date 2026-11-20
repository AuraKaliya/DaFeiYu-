#pragma once
#include "partner_feed.hpp"
#include "game_work.hpp"
#include <dafeiyu/agent_abi_v2.h>
#include <dafeiyu/companion_abi_v2.h>
#include <dafeiyu/mailbox_abi_v2.h>
#include <array>
#include <deque>

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
    // 在真实原生票据已经退出时归档原效果，收件 ACK 与游戏动作回执独立。
    void record(std::uint64_t session, std::uint64_t invocation, std::string_view skill, uint32_t effect, std::string output) noexcept;
    // 保留原采集候选，只允许当前插件角色创建确认。
    bool confirm_collect(std::uint64_t session, std::uint64_t request, std::string_view envelope);
    // 查询原候选的批准与重新核验结果，空字符串表示确定不执行，空可选值表示等待玩家。
    std::optional<std::string> confirm_result(std::uint64_t request) const;
    // 归还原确认并保存其终态，迟到回信可重读原决定。
    void confirm_exited(std::uint64_t request) noexcept;
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
    // 按序尝试投递一份已准备的事实，容量拒收时保留同一发送编号。
    void flush();
    // 把本轮已经退出的游戏动作转换为独立信箱事实。
    void report_actions();
    // 在本地有界保留结构化事实，等本体接纳后归还。
    void publish(std::string_view category, const nlohmann::json& payload);
    // 推进原世界接管并发布其真实释放结果。
    bool release_game(const BridgeConfig& config, std::uint64_t instance);
    // 按原配对身份处理游戏回复、主信箱回信或业务超时。
    void advance_confirmation();
    // 在宿主绑定的入站信箱读取至多一条请求并按分类处理。
    void intake(PartnerWork work);
    // 核对原收件的世界、任务和玩家，处理本体委托或只读询问。
    void intake_task(std::string_view category, const nlohmann::json& body, PartnerWork work);
    // 按原信箱请求处理批准和游戏投递指令。
    void intake_confirmation(bool present, const nlohmann::json& body);
    // 依据主侧选择或本地策略向同一玩家呈现原问题，并保留实际发言回执。
    void advance_confirmation_presentation(const BridgeConfig& config, std::uint64_t instance, bool closing);

    PartnerFeed feed_;
    const dfy_v2_agent_api* agent_ = nullptr;
    const dfy_v2_mailbox_api* mail_ = nullptr;
    const dfy_v2_mailbox_text_api* mail_text_ = nullptr;
    const dfy_v2_reference_api* refs_ = nullptr;
    std::uint64_t sent_ = 0;
    struct Frame { std::string_view category; std::string body; };
    std::deque<Frame> outbound_;
    struct Delegation { std::uint64_t id; std::string world, text; };
    std::optional<Delegation> awaiting_delegation_, active_delegation_;
    std::uint64_t last_task_ = 0;
    struct ActionReceipt { std::uint64_t id; std::string_view skill; uint32_t effect; std::string body; };
    std::array<ActionReceipt,32> actions_{};
    std::size_t action_count_ = 0;
    struct Release {
        std::string world, id, arguments;
        std::unique_ptr<PipeExchange> pipe;
        std::chrono::steady_clock::time_point deadline, next;
        bool done = false;
    };
    std::optional<Release> release_;
    std::uint64_t release_sequence_ = 0;
    struct Confirmation {
        std::uint64_t request, order, approved_order = 0;
        std::string world, player, question;
        nlohmann::json arguments, decision;
        std::chrono::steady_clock::time_point deadline;
        bool ask_game = false, presentation_attempted = false;
    };
    std::optional<Confirmation> confirmation_;
    struct Presentation {
        std::uint64_t request, order;
        std::string world, player;
        std::unique_ptr<GameWork> work;
    };
    std::optional<Presentation> presentation_;
    std::array<nlohmann::json,8> decisions_{};
    std::size_t next_decision_ = 0;
    // 裁定唯一原候选，结果一旦发布便不被迟到消息覆盖。
    void confirm_once(std::string_view status, std::string_view source, std::uint64_t approved_order = 0);
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
