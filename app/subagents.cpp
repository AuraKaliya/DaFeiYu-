#include "subagents.hpp"
#include <dafeiyu/plugin_hooks.hpp>
#include <algorithm>

namespace dfy::app {
struct Subagents::Slot {
    // 保存原实例弱引用并建立独立的 Agent 上下文。
    Slot(const std::shared_ptr<plugins::PluginInstance>& plugin, const model::Config& config,
        const std::shared_ptr<events::BroadcastChannel>& events, const model::Backend& backend)
        : owner(plugin->generation()), instance(plugin), session(config,events,backend) {}
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
};

// 固定正式宿主的服务来源，各槽单独创建上下文。
Subagents::Subagents(plugins::PluginManager& plugins, model::Config config, std::shared_ptr<events::BroadcastChannel> events,
    model::Backend backend, agent::RunVersionLabels labels, agent::ToolClock clock)
    : plugins_(plugins), config_(std::move(config)), events_(std::move(events)), backend_(std::move(backend)),
      labels_(std::move(labels)), clock_(std::move(clock)) {}

// 释放槽位，原会话仍负责协作取消和等待正在使用的资源。
Subagents::~Subagents() = default;

// 在独立槽位内创建上下文并签发原会话地址。
runtime::DomainId Subagents::open(const std::shared_ptr<plugins::PluginInstance>& instance) {
    if (stopping_ || !instance || (instance->phase() != plugins::InstancePhase::created &&
        instance->phase() != plugins::InstancePhase::active)) throw std::invalid_argument("subagent: owner unavailable");
    const auto free = std::ranges::find_if(slots_,[](const auto& slot) { return !slot; });
    if (free == slots_.end()) throw std::length_error("subagent: session capacity exhausted");
    *free = std::make_unique<Slot>(instance,config_,events_,backend_);
    return (*free)->session.id();
}

// 核对原所有者与会话地址后提供内部槽位。
Subagents::Slot& Subagents::find(runtime::DomainId owner, runtime::DomainId id) const {
    for (const auto& slot : slots_)
        if (slot && slot->owner == owner && slot->session.id() == id) return *slot;
    throw std::invalid_argument("subagent: session unavailable");
}

// 接纳一条有界输入，保留唯一递增请求号。
// 步骤：核对身份与空闲 → 校验正文和工具范围 → 发布排队状态。
std::uint64_t Subagents::submit(runtime::DomainId owner, runtime::DomainId id, SubagentInput input) {
    auto& slot = find(owner,id);
    if (stopping_ || slot.closing || slot.queued || slot.phase == SubagentPhase::running)
        throw std::logic_error("subagent: session busy or closing");
    if (input.prompt.empty() || input.prompt.size() > 64*1024 || input.instructions.size() > 8*1024 ||
        input.binding.size() > 8*1024 || input.tools.size() > 16 || slot.request == UINT64_MAX ||
        std::ranges::any_of(input.tools,[](const auto& id) { return id.empty() || id.size() > 128; }))
        throw std::invalid_argument("subagent: input outside limits");
    slot.queued.emplace(std::move(input));
    slot.answer.clear();
    slot.phase = SubagentPhase::queued;
    return ++slot.request;
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
    slot.phase = SubagentPhase::cancelled;
    return true;
}

// 标记关停，由宿主推进真实取消与退出。
void Subagents::close(runtime::DomainId owner, runtime::DomainId id) { find(owner,id).closing = true; }

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
    options.now = clock_;
    options.version_labels = labels_;
    options.system_instructions = std::move(input.instructions);
    options.extension = plugins::plugin_controls(plugins_,slot.owner);
    if (!input.tools.empty()) {
        auto catalog = std::make_shared<agent::ToolCatalog>(64);
        slot.registration = std::make_unique<plugins::PluginTools>(instance,catalog);
        options.tools = catalog->snapshot(input.tools,input.allow_changes).bind_context(input.binding);
    }
    slot.active = slot.session.begin(input.prompt,std::move(options));
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
            catch (...) { slot->phase = SubagentPhase::failed; slot->registration.reset(); }
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
                slot->registration.reset();
            }
        }
        if (slot->closing && slot->session.closed()) slot.reset();
    }
}
}
