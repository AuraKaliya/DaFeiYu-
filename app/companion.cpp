#include "companion.hpp"

namespace dfy::app {
namespace {
// 要求实例自身提供经工具目录核验的角色声明。
plugins::CompanionOffer require_offer(plugins::PluginInstance& instance, const agent::ToolCatalog& catalog) {
    auto offer = plugins::companion_offer(instance, catalog);
    if (!offer) throw std::invalid_argument("companion: plugin offers no companion role");
    return std::move(*offer);
}
}

// 为原插件建立宿主拥有的独立上下文，构造失败自动撤销临时注册。
// 步骤：登记当前实例工具 → 验证可选角色声明 → 创建独立会话。
CompanionSession::CompanionSession(const std::shared_ptr<plugins::PluginInstance>& instance, const model::Config& config,
    const std::shared_ptr<events::BroadcastChannel>& channel, const model::Backend& backend, const agent::RunVersionLabels& labels,
    agent::ToolClock clock)
    : instance_(instance), package_(instance->plugin_id()), catalog_(std::make_shared<agent::ToolCatalog>()),
      registration_(instance, catalog_), offer_(require_offer(*instance, *catalog_)), session_(config, channel, backend),
      conversation_(session_, labels), clock_(std::move(clock)) {}

// 重新核对原代次与活动状态，弱引用不阻止空闲 DLL 卸载。
bool CompanionSession::bound() const noexcept {
    const auto instance = instance_.lock();
    return instance && instance->generation() == offer_.generation && instance->phase() == plugins::InstancePhase::active;
}

// 按插件声明的只读入口向游戏中的同一角色委托一个回合。
// 步骤：核对会话与来源 → 固定本轮工具和时钟 → 接纳后清理上一回执。
agent::TurnDiagnostics CompanionSession::submit(const std::string& prompt) {
    if (stopping_ || !bound()) throw std::logic_error("companion: binding unavailable");
    agent::RunOptions options;
    options.now = clock_;
    options.tools = catalog_->snapshot(std::span(&offer_.state_tool, 1), false);
    const auto admitted = conversation_.submit(prompt, std::move(options));
    result_.reset();
    return admitted;
}

// 从真实状态同步绑定、推进子运行并投递至多一条待处理消息。
// 步骤：复核来源 → 结算原回合并回报 → 空闲时启动当前世界的委托或询问。
void CompanionSession::advance() {
    if (!stopping_ && !bound()) close();
    reader_.advance();
    sync_binding();
    inbox_.advance(*catalog_, clock_);
    if (!stopping_ && inbox_.status().enabled && !reader_.state().pending && clock_() >= next_refresh_) {
        refresh();
        next_refresh_ = clock_() + std::chrono::seconds(1);
    }
    if (auto completed = conversation_.advance()) result_ = std::move(completed);
    if (visit_work_ && visit_ && visit_work_->diagnostics().exit != runtime::OperationState::pending) {
        verify_visit(*visit_, *visit_work_, visit_tool_);
        visit_work_.reset();
    }
    if (result_ && delivering_ && !visit_work_) {
        finish_message(*delivering_, &*result_);
        delivering_.reset();
    }
    if (!stopping_ && !queued_ && !delivering_ && !conversation_.busy() && address_) accept_player();
    if (queued_ && !delivering_ && !conversation_.busy()) {
        try {
            if (visit_ && visit_->task == queued_->revision) start_visit();
            else submit(queued_->text);
            delivering_ = std::move(queued_);
        } catch (const std::exception&) {
            if (visit_ && visit_->task == queued_->revision) visit_->phase = VisitPhase::failed;
            finish_message(*queued_, nullptr);
        }
        queued_.reset();
    }
}

// 先撤销新工具接纳，再关闭会话并保留实际退出责任。
void CompanionSession::close() {
    if (stopping_) return;
    stopping_ = true;
    registration_.revoke();
    reader_.revoke();
    sync_binding();
    conversation_.close();
}

// 从实际注册中重新取得只读范围，不能向被替换的实例读取。
std::uint64_t CompanionSession::refresh() {
    if (stopping_ || !bound()) throw std::logic_error("companion: binding unavailable");
    return reader_.refresh(catalog_->snapshot(std::span(&offer_.state_tool, 1), false), clock_);
}

// 换世界、断线及撤销时封闭旧信箱，迟到结果仍可归还原运行资源。
// 步骤：核对最后观察 → 撤销原地址和待投递责任 → 为当前真实来源签发新代次。
void CompanionSession::sync_binding() {
    const auto& observation = reader_.state();
    const bool live = !stopping_ && observation.link == CompanionLink::available && observation.last.has_value();
    if (live && address_ && address_->world == observation.last->generation && actor_ == observation.last->actor) return;
    if (address_) {
        messages_.bind(std::nullopt);
        address_.reset();
        inbox_.bind(std::nullopt);
        queued_.reset();
        if (delivering_) conversation_.cancel();
        if (visit_ && visit_->phase == VisitPhase::queued) visit_->phase = VisitPhase::withdrawn;
        delivering_.reset();
    }
    if (live) {
        CompanionAddress fresh{id(), offer_.generation, observation.last->generation, runtime::next_domain()};
        actor_ = observation.last->actor;
        messages_.bind(fresh);
        inbox_.bind(fresh);
        address_ = std::move(fresh);
    }
}

// 只从已校验的玩家收件窗口创建业务委托，普通对话不取得工具范围。
void CompanionSession::accept_player() {
    const auto entry = inbox_.take();
    if (!entry || entry->visit_place.empty()) return;
    CompanionVisit visit{*address_, entry->sequence, 0, inbox_.status().owner, entry->text, entry->visit_place};
    visit.task = send(*address_, CompanionMessageKind::delegation, messages_.next(CompanionSide::primary), 0, entry->text);
    visit_ = std::move(visit);
}

// 从原始玩家消息生成固定参数，允许模型仅调用一次已授权的查看工具。
// 步骤：核对原绑定 → 固定工具与原参数 → 提交独立回合并保留实际工作。
void CompanionSession::start_visit() {
    if (!bound() || !address_ || visit_->address != *address_) throw std::logic_error("companion: expired visit");
    const auto args = visit_arguments(*visit_);
    agent::RunOptions options;
    options.now = clock_;
    options.tools = catalog_->snapshot(std::span(&visit_tool_, 1), true).bind_arguments(visit_tool_, args);
    options.policy.tool_limit = 1;
    const auto input = visit_->text + "\n宿主为原玩家委托固定的查看参数\n" + args;
    const auto turn = conversation_.submit(input, std::move(options));
    visit_work_ = conversation_.current_;
    visit_->run = agent::RunId{turn.session, turn.turn};
    visit_->phase = VisitPhase::submitted;
    result_.reset();
}

// 根据当前实例注册的用途开启私有收件，原游戏水位以前的聊天不补作委托。
// 步骤：校验本实例 → 查询只读收件与受控查看 → 加入宿主定期推进。
void CompanionSession::listen() {
    if (stopping_ || !bound()) throw std::logic_error("companion: inactive player source");
    const auto instance = instance_.lock();
    const auto inbox = plugins::companion_route(*instance, *catalog_, "player.inbox", false);
    const auto visit = plugins::companion_route(*instance, *catalog_, "visit.inspect", true);
    if (!inbox || !visit) throw std::invalid_argument("companion: player route not available");
    visit_tool_ = *visit;
    inbox_.enable(*inbox);
}

// 保留一条待处理的主侧消息，原命令重试不重复启动模型。
// 步骤：校验身份和待处理容量 → 登记原意图 → 把新消息移交宿主推进。
std::uint64_t CompanionSession::send(const CompanionAddress& address, CompanionMessageKind kind, std::uint64_t command,
    std::uint64_t reply_to, const std::string& text) {
    if (stopping_ || !bound() || !address_ || address != *address_) throw std::invalid_argument("companion: stale address");
    const bool fresh = command == messages_.next(CompanionSide::primary);
    if (fresh && (queued_ || (delivering_ && kind == CompanionMessageKind::delegation)))
        throw std::logic_error("companion: message delivery pending");
    auto message = messages_.post(address, CompanionSide::primary, kind, command, reply_to, text);
    const auto revision = message.revision;
    if (fresh) queued_.emplace(std::move(message));
    return revision;
}

// 只把当前地址的输出关联到原任务或原询问，异常回执独立标记。
void CompanionSession::finish_message(const CompanionMessage& original, const CompletedTurn* turn) {
    if (!address_ || original.address != *address_) return;
    if (visit_ && visit_->task == original.revision) {
        const bool observed = visit_->phase == VisitPhase::observed;
        const bool arrived = visit_->phase == VisitPhase::arrived;
        messages_.post(*address_, CompanionSide::game, (observed || arrived) ? CompanionMessageKind::report : CompanionMessageKind::failed,
            messages_.next(CompanionSide::game), original.revision, observed ? "已到达地标并发现目标" : arrived ? "已到达地标，暂未发现目标" : "查看未取得到达证据");
        return;
    }
    const bool success = turn && turn->status.result == runtime::OperationState::succeeded &&
        !turn->error && !turn->answer.empty() && turn->answer.size() <= 4096;
    const auto kind = original.kind == CompanionMessageKind::inquiry ? CompanionMessageKind::answer : CompanionMessageKind::report;
    messages_.post(*address_, CompanionSide::game, success ? kind : CompanionMessageKind::failed,
        messages_.next(CompanionSide::game), original.revision, success ? turn->answer : "子会话未能交付本次答复");
}

// 获取可跨界面复制的最新回合状态。
CompanionStatus CompanionSession::status() const {
    return {id(), offer_.generation, package_, offer_.state_topic, stopping_, closed(), conversation_.status()};
}

// 仅返回仍与原回合匹配的可信结算结果。
std::optional<CompletedTurn> CompanionSession::result(std::uint64_t turn) const {
    if (result_ && result_->status.turn == turn) return result_;
    return {};
}
}
