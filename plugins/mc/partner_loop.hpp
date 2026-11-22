#pragma once
#include "follow_attention.hpp"
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
#include <map>

namespace dfy::mc {
struct PartnerWork { std::uint64_t invocation = 0; std::string_view skill; };
// Minecraft 插件拥有事件处理、角色规则与游戏技能，模型工具循环借用宿主子会话。
class PartnerLoop {
  public:
    // 载入时登记求助和游戏渠道反馈的解析器。
    uint32_t register_questions(dfy_v2_query_interface_fn host);
    // 协商宿主子会话能力并启用原玩家入口。
    uint32_t listen(dfy_v2_query_interface_fn host, const BridgeConfig& config);
    // 停止读取并请求取消原在途回合。
    void stop() noexcept;
    // 在插件槽被回收时归还其私有的只读连接和事件。
    void reset() noexcept;
    // 非等待推进配对读取与原子会话，一个步骤最多接纳一个回合。
    void advance(const BridgeConfig& config, std::uint64_t instance, PartnerWork work, bool body_busy);
    // 新事实或世界变化时拦住旧会话尚未开始的步骤，保留已接纳动作的收尾。
    bool permits(std::uint64_t session) const noexcept;
    // 在本轮已交出一次性工作时将下次模型请求交还给外层调度。
    bool waits_for_work() const noexcept { return wait_for_work_; }
    // 同类计划两次失败后，只再给原回合一次模型结算机会。
    bool permits_model_step(std::uint64_t session, std::uint64_t step) noexcept;
    // 返回当前配对的前台动作门槛，空值表示可以继续验证动作。
    std::string_view front_barrier(std::uint64_t session) const noexcept;
    // 原工作已接纳后，关闭被新原话和观察共同证实的局内方块追问。
    void resolve_block_question(std::uint64_t item, std::string_view envelope);
    // 登记当前玩家指向的后台观察，额度用完后保留追问入口。
    bool admit_hit_observation(std::uint64_t session, const nlohmann::json& args);
    // 给当前对话或回执分配唯一的玩家发言机会。
    bool reserve_reply(std::uint64_t session, std::string_view work);
    // 判断同一任务仍缺少同一位玩家对局内追问的答复。
    bool waits_for_player(std::uint64_t session, std::uint64_t resolves = 0) const noexcept;
    // 返回指向核对结果，空值表示本次无法把动作关联到可信命中。
    std::optional<bool> pointed_dig(std::uint64_t session, std::uint64_t message, std::uint64_t item,
        nlohmann::json& args) const;
    // 判断原玩家指令是否只允许明确绑定的单格破坏。
    bool requires_pointed_dig(std::uint64_t session) const;
    // 有后继动作的追问只能由保留原依赖的计划消费。
    bool has_pickup_continuation(std::uint64_t item) const noexcept;
    // 在真实原生票据已经退出时归档原效果，收件 ACK 与游戏动作回执独立。
    void record(std::uint64_t session, std::uint64_t invocation, std::string_view skill, uint32_t effect,
        std::string work, std::string output) noexcept;
    // 接纳已经由游戏启动且尚未退出的原工作，接管轮询和停止。
    void adopt_motion(std::uint64_t invocation, std::string_view skill, const std::string& work,
        std::string_view envelope, std::unique_ptr<GameWork>& action, bool wait, bool pointed);
    // 给跟随期间的直接一次性动作登记优先步骤。
    bool interrupt_follow(std::uint64_t session, std::uint64_t invocation, const std::string& work,
        std::string_view skill, std::string_view envelope, bool wait, bool pointed);
    // 判断角色的移动控制是否仍由原工作持有。
    bool has_motion() const noexcept { return motion_.has_value(); }
    // 有待执行步骤时，直接动作不得越过队列。
    bool has_queued_action() const noexcept { return !queued_.empty() || (plan_ && plan_->active); }
    // 执行当前原子会话的局内任务或工具目录操作。
    nlohmann::json local_skill(std::uint64_t session, std::uint64_t invocation, std::string_view skill, std::string_view envelope,
        bool body_busy);
    // 在动作接纳前把当前原话目标绑定到放置参数。
    bool bind_placement(std::uint64_t session, std::string& envelope, bool whole_target = false) const;
    // 给当前子回合的采掘命令附上任务持有的取材范围。
    void bind_collection(std::uint64_t session, std::string& envelope) const;
    // 判断来自本体委托的当前可变更技能是否需要强制确认。
    bool delegated(std::uint64_t session) const noexcept { return session == session_ && active_delegation_.has_value(); }
    // 为游戏动作固定原参数和确认渠道，只允许当前插件角色提出。
    bool confirm_action(std::uint64_t session, std::uint64_t request, std::string_view skill, std::string_view envelope,
        std::string work = {});
    // 防止同技能重提绕过已登记的局内候选。
    bool awaiting_approval(std::string_view skill) const noexcept;
    // 对仍待确认或历史上受确认约束的同一动作保持约束。
    bool requires_approval(std::string_view skill, const nlohmann::json& arguments) const;
    // 查询原候选的批准与重新核验结果，空字符串表示确定不执行，空可选值表示等待玩家。
    std::optional<std::string> confirm_result(std::uint64_t request) const;
    // 归还原确认并保存其终态，迟到回信可重读原决定。
    void confirm_exited(std::uint64_t request) noexcept;
    // 查询已绑定玩家和实际回合水位。
    dfy_v2_partner_status status() const noexcept;
    // 区分私有收件可读与角色动作就绪。
    dfy_v2_partner_execution execution_status() const noexcept;
  private:
    // 当前回合实际退出后可发布原结算，或请求取消并继续等待。
    bool settle(const BridgeConfig& config, std::uint64_t instance, bool cancel);
    // 以原游戏工作提交玩家可见的最终文本，等待实际发言回执。
    bool settle_reply(const BridgeConfig& config, std::uint64_t instance, std::string_view answer, bool cancel);
    // 用原游戏游标、新事件或局内任务信号构造一轮，接纳后消费原件。
    void submit(std::string_view wake);
    // 分组查看已实现能力，并安排在下一轮更换可见的详细契约。
    nlohmann::json discover(const nlohmann::json& arguments);
    // 合并查询选中的类别与已知任务目标所需的类别。
    std::bitset<categories.size()> scope() const noexcept;
    // 展开当前的原话目标，并为每格沿用放置确认约束。
    bool bind_fill(nlohmann::json& arguments) const;
    // 仅保留有限的真人发言及已经实际投递的回复。
    void remember_dialogue(std::string_view speaker, std::uint64_t order, const nlohmann::json& text);
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
    // 同一种确定性参数失败达到两次后，让出本轮以等待新事实。
    void limit_repair(std::string code);
    // 读回脱离回合的在途动作，只有真实结果或失联才归还动作槽。
    void advance_motion(bool stopping);
    // 从同一跟随工作的读回捕获一次异常或恢复。
    void track_follow(const nlohmann::json& evidence, bool terminal = false);
    // 组装供信箱事实和模型上下文使用的当前异常。
    nlohmann::json follow_attention_context() const;
    // 根据当前原话切换任务并移交旧动作的实际退出责任。
    nlohmann::json switch_task(const nlohmann::json& arguments);
    // 接纳、清空或替换当前任务修订下尚未执行的步骤。
    nlohmann::json queue_actions(std::uint64_t invocation, const nlohmann::json& arguments);
    // 在角色空闲且原前置工作已核实后提交一个步骤。
    void advance_queue(const BridgeConfig& config, bool closing, bool body_busy);
    // 按最新游标恢复原跟随的下一段执行。
    void resume_follow(const BridgeConfig& config, bool body_busy);
    // 仅在跟随实际占用角色时请求其退出当前执行段。
    void pause_follow() noexcept;
    // 批量结算未执行的步骤并释放队列。
    void clear_queue(std::string_view reason, std::uint64_t group = 0);
    // 接纳当前目标下的短任务图，验证通过后才替换上次任务图。
    nlohmann::json submit_plan(std::uint64_t invocation, const nlohmann::json& arguments);
    // 记录未接纳的原计划和可修订状态，不创建执行等待。
    nlohmann::json report_plan_attempt(std::uint64_t invocation, nlohmann::json result);
    // 读取同层各节点的真实回执，或将下层节点交给前后台执行器。
    void advance_plan(const BridgeConfig& config, bool closing);
    // 标注仍未执行的节点，并等待在途工作实际退出。
    void cancel_plan(std::string_view reason);
    // 停止旧任务的前台工作，保留原号与实际退出交接。
    void hold_task_motion();
    // 检查计划节点是否受当前候选确认约束。
    bool plan_action_requires_approval(std::string_view skill, const nlohmann::json& arguments) const;
    // 用原节点和真实退出结果结算，不重复发布已终结节点。
    void finish_plan_node(std::size_t index, bool ok, const nlohmann::json& result);
    // 返回任务图及各节点状态，供普通聊天与任务续行共用。
    nlohmann::json plan_context() const;
    // 在本地有界保留结构化事实，等本体接纳后归还。
    void publish(std::string_view category, const nlohmann::json& payload);
    // 推进原世界接管并发布其真实释放结果。
    bool release_game(const BridgeConfig& config, std::uint64_t instance);
    // 按原配对身份处理本地游戏回复或原动作时限。
    void advance_confirmation();
    // 推进插件持有的候选，允许执行时接入原游戏工作。
    void advance_confirmed_work(const BridgeConfig& config, bool closing, bool body_busy);
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
    // 推进选定渠道的实际呈现与玩家答复。
    void advance_question(const BridgeConfig& config, std::uint64_t instance, bool closing);
    // 接纳原问题的结果，并准备下一轮上下文。
    void complete_question(std::string_view status, std::string answer, std::string_view channel = {});
    // 关闭已由当前原话和动作证据共同确认的局内追问。
    void complete_pointed_question();
    // 检查原候选在执行时使用的世界、玩家及游标。
    bool confirmation_context_current(std::uint64_t order) const;
    // 保留并按序提交原生渠道事件。
    void flush_question_events();

    struct Question {
        std::uint64_t item, order, ask = 0;
        std::string world, player, text, path;
        std::chrono::steady_clock::time_point local_answer_deadline;
        bool started = false, presented = false, dispatched = false, forwarded = false;
        bool approval = false;
        bool expired = false;
        std::uint64_t event_domain = 0, event_sequence = 0, answer_order = 0;
        std::uint64_t task_revision = 0;
        nlohmann::json options, after_pickup;
        std::string origin;
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
        bool accepted = true;
        std::string pickup;
        bool queued = false;
        std::optional<std::size_t> plan_node;
        bool pointed = false;
        std::uint64_t task = 0, revision = 0;
    };
    std::optional<Motion> motion_;
    struct Handoff {
        std::string work;
        nlohmann::json receipt;
        bool rechecked = false;
    };
    std::optional<Handoff> handoff_;
    bool awaiting_restarted_plan_ = false;
    struct Follow {
        std::uint64_t order, request, invocation, segment = 1;
        std::string id;
        nlohmann::json arguments;
        bool pausing = false, suspended = false, needs_route_check = false;
        FollowAttention attention;
        bool attention_pending = false;
    };
    std::optional<Follow> follow_;
    struct AttentionTurn { std::string work; std::uint64_t episode; };
    std::optional<AttentionTurn> attention_turn_;
    struct QueuedAction {
        std::uint64_t order, request, invocation, revision;
        std::string work;
        std::string_view skill;
        nlohmann::json arguments;
        std::string pickup;
        bool interrupt = false;
        bool notify = false;
        std::optional<std::size_t> plan_node;
        bool pointed = false;
    };
    std::deque<QueuedAction> queued_;
    struct PlanNode {
        enum class State { pending, running, done, failed, skipped };
        std::string id, work;
        std::string_view skill;
        nlohmann::json arguments, result;
        std::string pickup;
        std::vector<std::size_t> after;
        std::size_t layer = 0;
        State state = State::pending;
        std::unique_ptr<GameWork> reading;
    };
    struct Plan {
        std::uint64_t id, revision, order, request, supersedes;
        std::vector<PlanNode> nodes;
        std::size_t layer = 1;
        bool active = true;
        std::string cancellation;
    };
    std::optional<Plan> plan_;
    nlohmann::json plan_receipts_ = nlohmann::json::array();
    nlohmann::json plan_rejection_;
    unsigned plan_failures_ = 0;
    bool plan_repair_exhausted_ = false;
    std::uint64_t plan_reply_step_ = 0;
    bool replacing_motion_ = false;
    std::deque<std::pair<std::string,nlohmann::json>> guarded_actions_;
    struct WorkSettlement { std::uint64_t invocation; nlohmann::json receipts; };
    std::deque<WorkSettlement> settlements_;
    bool wait_for_work_ = false;
    std::map<std::string,unsigned> repair_failures_;
    std::map<std::uint64_t,unsigned> hit_queries_;
    // 按调用组累积实际终态，独立于玩家事件的收取顺序。
    void settle_work(std::uint64_t invocation, nlohmann::json fact);
    // 只选出已无在途动作或待执行步骤的调用组。
    std::deque<WorkSettlement>::const_iterator ready_settlement() const;
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
    std::unique_ptr<GameWork> final_reply_;
    struct ReplyReservation { std::string work; bool unsent = false; };
    ReplyReservation reply_, rejection_reply_;
    bool question_requested_ = false;
    bool final_text_for_player_ = false;
    struct Release {
        std::string world, id, arguments;
        std::unique_ptr<PipeExchange> pipe;
        std::chrono::steady_clock::time_point deadline, next;
        bool done = false;
    };
    std::optional<Release> release_;
    std::uint64_t release_sequence_ = 0;
    struct ApprovalWork { std::uint64_t session, turn, revision; std::string id; };
    struct Confirmation {
        std::uint64_t request, order, approved_order = 0;
        std::string world, player, skill, question;
        nlohmann::json arguments, decision;
        std::chrono::steady_clock::time_point deadline;
        bool ask_game = false, presentation_attempted = false, presented = false;
        std::optional<ApprovalWork> work;
    };
    std::optional<Confirmation> confirmation_;
    nlohmann::json approval_result_;
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
    std::deque<nlohmann::json> dialogue_;
    std::uint64_t reviewed_ = 0;
    bool stopped_work_ = false;
    uint32_t phase_ = DFY_V2_PARTNER_CLOSED;
};
}
