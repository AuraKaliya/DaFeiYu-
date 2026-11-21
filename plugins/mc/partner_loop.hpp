#pragma once
#include "partner_feed.hpp"
#include "game_work.hpp"
#include "partner_task.hpp"
#include "partner_skills.hpp"
#include <dafeiyu/agent_abi_v2.h>
#include <dafeiyu/companion_abi_v2.h>
#include <dafeiyu/mailbox_abi_v2.h>
#include <dafeiyu/routed_event_abi_v2.h>
#include <array>
#include <bitset>
#include <deque>

namespace dfy::mc {
struct PartnerWork { std::uint64_t invocation = 0; std::string_view skill; };
// Minecraft 插件拥有事件处理、角色规则与游戏技能，模型工具循环借用宿主子会话。
class PartnerLoop {
  public:
    // 载入时登记疑问升级和游戏渠道反馈的解析器。
    uint32_t register_questions(dfy_v2_query_interface_fn host);
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
    void record(std::uint64_t session, std::uint64_t invocation, std::string_view skill, uint32_t effect,
        std::string work, std::string output) noexcept;
    // 接纳已经由游戏启动且尚未退出的原工作，接管轮询和停止。
    void adopt_motion(std::uint64_t invocation, std::string_view skill, const std::string& work, std::unique_ptr<GameWork>& action);
    // 判断角色的移动控制是否仍由原工作持有。
    bool has_motion() const noexcept { return motion_.has_value(); }
    // 执行当前原子会话的局内任务或工具目录操作。
    nlohmann::json local_skill(std::uint64_t session, std::uint64_t invocation, std::string_view skill, std::string_view envelope);
    // 在动作接纳前把当前原话目标绑定到放置参数。
    bool bind_placement(std::uint64_t session, std::string& envelope) const;
    // 判断来自本体委托的当前可变更技能是否需要强制确认。
    bool delegated(std::uint64_t session) const noexcept { return session == session_ && active_delegation_.has_value(); }
    // 为游戏动作固定原参数和确认渠道，只允许当前插件角色提出。
    bool confirm_action(std::uint64_t session, std::uint64_t request, std::string_view skill, std::string_view envelope);
    // 查询原候选的批准与重新核验结果，空字符串表示确定不执行，空可选值表示等待玩家。
    std::optional<std::string> confirm_result(std::uint64_t request) const;
    // 归还原确认并保存其终态，迟到回信可重读原决定。
    void confirm_exited(std::uint64_t request) noexcept;
    // 查询已绑定玩家和实际回合水位。
    dfy_v2_partner_status status() const noexcept;
  private:
    // 当前回合实际退出后可发布原结算，或请求取消并继续等待。
    bool settle(bool cancel);
    // 用原游戏游标、新事件或局内任务信号构造一轮，接纳后消费原件。
    void submit(std::string_view wake);
    // 分组查看已实现能力，并安排在下一轮更换可见的详细契约。
    nlohmann::json discover(const nlohmann::json& arguments);
    // 给已绑定且无新输入的游戏返回准备好的自主唤醒。
    std::string_view autonomous_wake() const;
    // 结束旧游戏绑定对应的 Agent 上下文。
    void retire();
    // 单独判断新事实是否应中止原生在途动作，过期判断不得中止替代动作。
    void review(PartnerWork work, bool closing);
    // 按序尝试投递一份已准备的事实，容量拒收时保留同一发送编号。
    void flush();
    // 把本轮已经退出的游戏动作转换为独立信箱事实。
    void report_actions();
    // 读回脱离回合的在途动作，只有真实结果或失联才归还动作槽。
    void advance_motion(bool stopping);
    // 在本地有界保留结构化事实，等本体接纳后归还。
    void publish(std::string_view category, const nlohmann::json& payload);
    // 推进原世界接管并发布其真实释放结果。
    bool release_game(const BridgeConfig& config, std::uint64_t instance);
    // 按原配对身份处理本地游戏回复或原动作时限。
    void advance_confirmation();
    // 在宿主绑定的入站信箱读取至多一条请求并按分类处理。
    void intake(PartnerWork work);
    // 核对原收件的世界、任务和玩家，处理本体委托或只读询问。
    void intake_task(std::string_view category, const nlohmann::json& body, PartnerWork work);
    // 依据主侧选择或本地策略向同一玩家呈现原问题，并保留实际发言回执。
    void advance_confirmation_presentation(const BridgeConfig& config, std::uint64_t instance, bool closing);
    // 建立原子会话的待答事项，原答案只在匹配当前世界时生效。
    nlohmann::json open_question(std::uint64_t id, const nlohmann::json& arguments);
    // 按事项原身份接收通用 ASK 渠道请求或结果。
    void intake_question(std::string_view category, const nlohmann::json& body, const dfy_v2_mailbox_message& message);
    // 推进本地等待、必要升级、实际呈现与玩家答复。
    void advance_question(const BridgeConfig& config, std::uint64_t instance, bool closing);
    // 接纳原问题的结果，并准备下一轮上下文。
    void complete_question(std::string_view status, std::string answer, std::string_view channel = {});
    // 检查原候选在执行时使用的世界、玩家及游标。
    bool confirmation_context_current(std::uint64_t order) const;
    // 保留并按序提交原生渠道事件。
    void flush_question_events();

    struct Question {
        std::uint64_t item, order, ask = 0;
        std::string world, player, text, path;
        std::chrono::steady_clock::time_point local, local_answer_deadline;
        bool initial_started = false, ask_started = false, presented = false, escalated = false, forwarded = false;
        bool approval = false;
        std::uint64_t event_domain = 0, event_sequence = 0, answer_order = 0;
    };
    const dfy_v2_routed_event_api* question_events_ = nullptr;
    std::uint64_t question_sequence_ = 0;
    std::deque<std::pair<std::string_view,nlohmann::json>> question_outbound_;
    std::optional<Question> question_;
    std::unique_ptr<GameWork> question_work_;
    nlohmann::json question_result_;
    bool question_wake_ = false;

    PartnerFeed feed_;
    PartnerTask task_;
    struct Motion {
        std::uint64_t order, session, request, invocation;
        std::string world, work;
        std::string_view skill;
        std::unique_ptr<GameWork> action;
        bool stopping = false, announced = false;
        std::string last_phase;
    };
    std::optional<Motion> motion_;
    bool motion_wake_ = false;
    std::bitset<categories.size()> expanded_;
    bool expansion_pending_ = false;
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
    struct ActionReceipt { std::uint64_t id; std::string_view skill; uint32_t effect; std::string work, body; };
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
        std::string world, player, skill, question;
        nlohmann::json arguments, decision;
        std::chrono::steady_clock::time_point deadline;
        bool ask_game = false, presentation_attempted = false;
    };
    std::optional<Confirmation> confirmation_;
    struct Presentation {
        std::uint64_t request, order;
        std::string world, player, work_id;
        std::unique_ptr<GameWork> work;
    };
    std::optional<Presentation> presentation_;
    // 裁定唯一原候选，结果一旦发布便不被迟到消息覆盖。
    void confirm_once(std::string_view status, std::string_view source, std::uint64_t approved_order = 0);
    std::string session_world_;
    std::uint64_t session_ = 0, request_ = 0, order_ = 0, settled_ = 0;
    struct Review {
        std::uint64_t session = 0, request = 0, origin = 0, work = 0, cursor = 0;
    } review_;
    nlohmann::json events_, recent_events_, previous_;
    std::uint64_t reviewed_ = 0;
    bool stopped_work_ = false;
    uint32_t phase_ = DFY_V2_PARTNER_CLOSED;
};
}
