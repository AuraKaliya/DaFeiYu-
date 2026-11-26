#include "subagents.hpp"
#include "binding_facts.hpp"
#include "run_log.hpp"
#include "persona.hpp"
#include <dafeiyu/digest.hpp>
#include <nlohmann/json.hpp>
#include <dafeiyu/plugin_hooks.hpp>
#include <algorithm>

namespace dfy::app {
struct Subagents::Slot {
    // 保存原实例弱引用并建立独立的 Agent 上下文。
    Slot(const std::shared_ptr<plugins::PluginInstance>& plugin, const model::Config& config,
        const std::shared_ptr<events::BroadcastChannel>& events, const model::Backend& backend, agent::RunCaptureSinks capture)
        : owner(plugin->generation()), instance(plugin), session(config,events,backend,4,std::move(capture)) {}
    const runtime::DomainId owner;
    std::weak_ptr<plugins::PluginInstance> instance;
    std::unique_ptr<plugins::PluginTools> registration;
    agent::Session session;
    std::optional<SubagentInput> queued;
    std::shared_ptr<agent::TurnRequest> active;
    std::uint64_t request = 0;
    SubagentPhase phase = SubagentPhase::idle;
    std::string answer;
    bool closing = false;
    bool compare_projection = false;
    BindingFacts facts;
    std::unique_ptr<BindingFacts> pending_facts;
    bool continuous_history = false;
    std::uint64_t projected_receipt = 0, pending_receipt = 0;
};
struct Subagents::WorkSlot {
    runtime::DomainId owner, session;
    BindingWorks works;
    bool retired = false;

    // 先结算原工作，只有水位推进才向诊断日志追加一次原回执。
    std::uint64_t settle(std::uint64_t epoch, const plugins::SubagentWorkFact& fact, const std::shared_ptr<RunLog>& log) {
        const auto before = works.watermark();
        const auto receipt = works.settle(epoch,fact);
        if (log && receipt > before) log->work_fact(owner,session,retired,fact,receipt);
        return receipt;
    }
};

// 固定正式宿主的服务来源，各槽单独创建上下文。
Subagents::Subagents(plugins::PluginManager& plugins, model::Config config, std::shared_ptr<events::BroadcastChannel> events,
    model::Backend backend, agent::RunVersionLabels labels, agent::ToolClock clock, const PersonaProfile& persona, std::shared_ptr<RunLog> log)
    : plugins_(plugins), config_(std::move(config)), events_(std::move(events)), backend_(std::move(backend)),
      labels_(std::move(labels)), clock_(std::move(clock)), persona_(persona), log_(std::move(log)) {}

// 释放槽位，原会话仍负责协作取消和等待正在使用的资源。
Subagents::~Subagents() = default;

// 在独立槽位内创建上下文并签发原会话地址。
runtime::DomainId Subagents::open(const std::shared_ptr<plugins::PluginInstance>& instance) {
    if (stopping_ || !instance || (instance->phase() != plugins::InstancePhase::created &&
        instance->phase() != plugins::InstancePhase::active)) throw std::invalid_argument("subagent: owner unavailable");
    const auto free = std::ranges::find_if(slots_,[](const auto& slot) { return !slot; });
    if (free == slots_.end()) throw std::length_error("subagent: session capacity exhausted");
    const bool compare = log_ && instance->plugin_id() == "mc.companion" &&
        std::ranges::none_of(slots_,[](const auto& slot) { return slot && slot->compare_projection; });
    *free = std::make_unique<Slot>(instance,config_,events_,backend_,log_ ? log_->sinks("plugin",compare) : agent::RunCaptureSinks{});
    (*free)->compare_projection = compare;
    return (*free)->session.id();
}

// 核对原所有者与会话地址后提供内部槽位。
Subagents::Slot& Subagents::find(runtime::DomainId owner, runtime::DomainId id) const {
    for (const auto& slot : slots_)
        if (slot && slot->owner == owner && slot->session.id() == id) return *slot;
    throw std::invalid_argument("subagent: session unavailable");
}

// 工作索引独立于模型资源，旧会话退役后仍须匹配原实例和原会话。
Subagents::WorkSlot* Subagents::work_slot(runtime::DomainId owner, runtime::DomainId id) const noexcept {
    for (const auto& work : work_slots_) if (work && work->owner==owner && work->session==id) return work.get();
    return nullptr;
}

// 接纳一条有界输入，保留唯一递增请求号。
// 步骤：核对身份与空闲 → 校验正文和工具范围 → 发布排队状态。
std::uint64_t Subagents::submit(runtime::DomainId owner, runtime::DomainId id, SubagentInput input) {
    auto& slot = find(owner,id);
    if (input.frame && !slot.closing && !stopping_) {
        const auto& ledger = slot.pending_facts ? *slot.pending_facts : slot.facts;
        if (const auto original = ledger.prior(input)) return original;
    }
    if (stopping_ || slot.closing || slot.queued || slot.phase == SubagentPhase::running)
        throw std::logic_error("subagent: session busy or closing");
    const auto persona_size = input.player_facing ? persona_.instructions.size()+(!input.instructions.empty() ? 1 : 0) : 0;
    if (input.prompt.empty() || input.prompt.size() > 64*1024 || input.instructions.size()+persona_size > 8*1024 ||
        input.binding.size() > 8*1024 || input.tools.size() > 16 || slot.request == UINT64_MAX ||
        std::ranges::any_of(input.tools,[](const auto& id) { return id.empty() || id.size() > 128; }))
        throw std::invalid_argument("subagent: input outside limits");
    std::unique_ptr<BindingFacts> pending;
    if (input.frame) {
        pending = std::make_unique<BindingFacts>(slot.facts);
        const bool baseline = input.frame->baseline || !slot.continuous_history;
        auto prompt = pending->apply(input,slot.request+1,baseline);
        const auto after = baseline ? 0 : slot.projected_receipt;
        // 原事实帧最多约 64 KiB，另给两个水位及数组字段名留出空间。
        constexpr std::size_t projected_limit = 128*1024;
        const auto* ledger = work_slot(owner,id);
        const auto through = ledger ? ledger->works.watermark() : 0;
        const auto works = ledger ? ledger->works.view(input.frame->epoch,after,projected_limit-prompt.size()-128) : nlohmann::json::array();
        if (!works.empty()) {
            auto data = nlohmann::json::parse(prompt);
            data["factFrame"]["workAfter"] = after;
            data["factFrame"]["workThrough"] = through;
            data["hostWorkReceipts"] = works;
            prompt = data.dump();
        }
        if (prompt.size() > projected_limit) throw std::length_error("subagent: projected input outside limit");
        input.prompt = std::move(prompt);
        input.replace_history = baseline;
        slot.pending_receipt = through;
    }
    slot.queued.emplace(std::move(input));
    slot.pending_facts = std::move(pending);
    slot.answer.clear();
    slot.phase = SubagentPhase::queued;
    return ++slot.request;
}

// 工作回执只能沿原所有者和作用域接纳，退役索引不持有模型或工具。
// 步骤：沿原身份查活动或退役记录 → 核对新工作的有效绑定 → 在有界索引中为首次接纳预留位置。
std::uint64_t Subagents::settle(runtime::DomainId owner, runtime::DomainId id, plugins::SubagentWorkFact fact) {
    auto* ledger = work_slot(owner,id);
    if (ledger && ledger->retired) return ledger->settle(0,fact,log_);
    auto& slot = find(owner,id);
    const auto epoch = slot.closing || stopping_ ? 0 : slot.facts.epoch();
    if (ledger) return ledger->settle(epoch,fact,log_);

    auto free = std::ranges::find_if(work_slots_,[](const auto& item) { return !item; });
    if (free==work_slots_.end()) {
        for (auto it=work_slots_.begin();it!=work_slots_.end();++it) if ((*it)->retired && (*it)->works.finalized() &&
            (free==work_slots_.end() || (*it)->session < (*free)->session)) free = it;
    }
    if (free==work_slots_.end()) throw std::length_error("subagent: unsettled binding capacity exhausted");
    auto next = std::make_unique<WorkSlot>();
    next->owner = owner; next->session = id;
    const auto receipt = next->settle(epoch,fact,log_);
    *free = std::move(next);
    return receipt;
}

// 先核对原实例和会话，再返回已留存的回执内容。
std::optional<std::string> Subagents::read_receipt(runtime::DomainId owner, runtime::DomainId id,
    std::uint64_t epoch, std::uint64_t receipt) const {
    const auto* ledger = work_slot(owner,id);
    return ledger ? ledger->works.read(epoch,receipt) : std::nullopt;
}

// 投影原请求状态和本次结算正文。
SubagentStatus Subagents::status(runtime::DomainId owner, runtime::DomainId id) const {
    const auto& slot = find(owner,id);
    return {slot.request,slot.phase,slot.answer};
}

// 排队请求直接撤销，在途工作请求协作退出。
bool Subagents::cancel(runtime::DomainId owner, runtime::DomainId id, std::uint64_t request) {
    auto& slot = find(owner,id);
    if (request != slot.request) return false;
    if (slot.active) return slot.active->request_cancel(runtime::CancelReason::owner_stop) != runtime::CancelDisposition::already_settled;
    if (!slot.queued) return false;
    slot.queued.reset();
    slot.pending_facts.reset();
    slot.phase = SubagentPhase::cancelled;
    return true;
}

// 标记关停，由宿主推进真实取消与退出。
void Subagents::close(runtime::DomainId owner, runtime::DomainId id) { find(owner,id).closing = true; }

// 回收原实例尚未发出的提交并标记全部会话停止。
void Subagents::close_owner(runtime::DomainId owner) noexcept {
    for (auto& ledger : work_slots_) if (ledger && ledger->owner == owner) ledger.reset();
    for (auto& slot : slots_) if (slot && slot->owner == owner) {
        if (slot->phase != SubagentPhase::running) { slot.reset(); continue; }
        slot->closing = true;
        slot->queued.reset();
    }
}

// 检查原所有者的排队或尚未退出的工作。
bool Subagents::busy(runtime::DomainId owner) const noexcept {
    return std::ranges::any_of(slots_,[owner](const auto& slot) {
        return slot && slot->owner == owner && (slot->queued || slot->phase == SubagentPhase::running);
    });
}

// 查询全部子会话槽位是否已释放。
bool Subagents::empty() const noexcept { return std::ranges::all_of(slots_,[](const auto& slot) { return !slot; }); }

// 关闭接纳，让后续推进负责收尾。
void Subagents::stop() noexcept { stopping_ = true; }

// 根据原实例工具注册构造有界的子回合。
// 步骤：接管排队输入 → 固定原实例权限和 Hook → 接纳独立 Agent 回合。
void Subagents::begin(Slot& slot, const std::shared_ptr<plugins::PluginInstance>& instance) {
    auto input = std::move(*slot.queued);
    slot.queued.reset();
    slot.phase = SubagentPhase::running;
    agent::RunOptions options;
    if (input.thinking == plugins::SubagentThinking::off) options.thinking = model::ThinkingPreference::off;
    if (input.thinking == plugins::SubagentThinking::low) options.thinking = model::ThinkingPreference::low;
    options.prompt_source = model::MessageSource::plugin;
    options.now = clock_;
    options.version_labels = labels_;
    const auto snapshot = plugins_.capture_hooks(slot.owner);
    options.version_labels.control_extension = snapshot.version;
    options.system_instructions = std::move(input.instructions);
    if (!options.system_instructions.empty())
        options.version_labels.prompt_template = "sha256:"+platform::sha256(options.system_instructions);
    if (input.player_facing) persona_.apply(options);
    options.extension = plugins::plugin_controls(snapshot,slot.owner);
    options.text_hook = plugins::plugin_text_hooks(snapshot,slot.owner);
    options.exit_observer = plugins::plugin_exit_hook(snapshot);
    if (!input.tools.empty()) {
        auto catalog = std::make_shared<agent::ToolCatalog>(64);
        slot.registration = std::make_unique<plugins::PluginTools>(instance,catalog);
        options.tools = catalog->snapshot(input.tools,input.allow_changes).bind_context(input.binding);
    }
    options.replace_history = input.replace_history;
    slot.active = slot.session.begin(input.prompt,std::move(options));
    slot.continuous_history = static_cast<bool>(slot.pending_facts);
    if (slot.pending_facts) {
        slot.facts = std::move(*slot.pending_facts);
        slot.pending_facts.reset();
        slot.projected_receipt = slot.pending_receipt;
    }
    if (log_) {
        try {
            const auto id = slot.active->run_id();
            log_->append("subagent.accepted",{{"session",id.session},{"run",id.sequence},{"plugin",instance->plugin_id()},
                {"instance",slot.owner},{"request",slot.request}});
        } catch (...) {}
    }
}

// 推进每个槽位，回执在实际退出后才变为终态。
// 步骤：核对关停与所有权 → 派发排队回合 → 推进原工作 → 回收退出责任。
void Subagents::advance() {
    for (auto& slot : slots_) {
        if (!slot) continue;
        const auto owner = slot->instance.lock();
        if (stopping_ || slot->closing || !owner || owner->phase() != plugins::InstancePhase::active) {
            slot->queued.reset();
            slot->session.close();
            slot->closing = true;
        } else if (slot->queued) {
            try { begin(*slot,owner); }
            catch (...) { slot->phase = SubagentPhase::failed; slot->pending_facts.reset(); slot->registration.reset(); }
        }
        if (slot->active) {
            slot->session.advance();
            const auto state = slot->active->diagnostics();
            if (state.exit != runtime::OperationState::pending) {
                slot->phase = state.result == runtime::OperationState::cancelled ? SubagentPhase::cancelled : SubagentPhase::failed;
                if (state.result == runtime::OperationState::succeeded) {
                    try { slot->answer = slot->active->answer(); slot->phase = SubagentPhase::succeeded; }
                    catch (...) { slot->answer.clear(); }
                }
                slot->active.reset();
                if (slot->phase != SubagentPhase::succeeded) slot->continuous_history = false;
                slot->registration.reset();
            }
        }
        if (slot->closing && slot->session.closed()) {
            if (auto* ledger = work_slot(slot->owner,slot->session.id())) ledger->retired = true;
            slot.reset();
        }
    }
}
}
