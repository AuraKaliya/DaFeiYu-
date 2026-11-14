#include <dafeiyu/plugin_instance.hpp>
#include <dafeiyu/plugin_config.hpp>

namespace dfy::plugins {
PluginInstance::State* PluginInstance::retained_ = nullptr;
std::size_t PluginInstance::resident_ = 0;
struct PluginInstance::State {
    // 在任何实例副作用前完成全部宿主分配、能力校验及配置读取。
    // 步骤：加载模块与服务 → 查询完整生命周期 → 配置事件、可选组及通知 → 登记宿主状态名额。
    State(const Manifest& manifest, std::shared_ptr<const EffectiveConfig> settings,
        std::shared_ptr<events::BroadcastChannel> channel, InstanceLimits limits, std::shared_ptr<events::ConsumerGroup> group)
        : module(manifest, limits.code_leases, limits.active_calls), host(limits.references, limits.readers), config(std::move(settings)), id(manifest.id) {
        lifecycle = module.query<dfy_v2_lifecycle_api>(DFY_V2_LIFECYCLE_INTERFACE, 1);
        if (!lifecycle) throw std::runtime_error("instance: lifecycle unavailable");
        {
            auto call = lifecycle->enter(); const auto& api = call.api();
            if (!api.create || !api.start || !api.request_stop || !api.poll_stop || !api.destroy) throw std::runtime_error("instance: incomplete lifecycle");
        }
        host.enable_events(std::move(channel), limits.requests);
        if (group) host.enable_group(std::move(group),limits.requests);
        host.enable_notifications(module.retain_code(), limits.notifications);
        ++resident_;
    }
    // 只在已安全回收后由控制器或保留表销毁，归还宿主状态名额。
    ~State() { --resident_; }
    // 在正确绑定和活动保护中调用状态入口，隔离错误正文及异常。
    template<class Action>
    uint32_t invoke(Action action) noexcept {
        try {
            auto binding = host.bind(); auto call = lifecycle->enter();
            dfy_v2_error diagnostic{};
            const auto status = action(call.api(), &diagnostic);
            if (diagnostic.length > DFY_V2_ERROR_CAPACITY || (!status && diagnostic.length)) return DFY_V2_FAILED;
            return status;
        } catch (...) { return DFY_V2_FAILED; }
    }
    // 保留每个已完成阶段，失败不销毁未知状态的实例或释放代码。
    // 步骤：拒绝活动及外部持有 → 关闭宿主入口 → 请求及查询停止 → 单次销毁 → 结束服务并释放模块。
    ShutdownResult shutdown() noexcept {
        try {
            if (phase == InstancePhase::unloaded) return ShutdownResult::closed;
            if (module.active_calls()) return ShutdownResult::busy;
            if (host.processing_busy()) return ShutdownResult::busy;
            if (module.code_leases() > (lifecycle ? 1 : 0) + host.code_holds()) return ShutdownResult::in_use;
            if (phase == InstancePhase::broken) return ShutdownResult::failed;
            if (!host.begin_shutdown()) { error = DFY_V2_FAILED; return ShutdownResult::failed; }
            if (phase != InstancePhase::destroyed) phase = InstancePhase::stopping;
            if (instance && !stop_requested) {
                const auto status = invoke([&](const auto& api, auto* e) { return api.request_stop(instance, e); });
                if (status) { error = status; return ShutdownResult::failed; }
                stop_requested = true;
            }
            if (instance && !stopped) {
                uint32_t result = 0;
                const auto status = invoke([&](const auto& api, auto* e) { return api.poll_stop(instance, &result, e); });
                if (status) { error = status; return ShutdownResult::failed; }
                if (result == DFY_V2_STOP_PENDING) return ShutdownResult::pending;
                if (result != DFY_V2_STOPPED) { error = DFY_V2_FAILED; return ShutdownResult::failed; }
                stopped = true;
            }
            if (instance) {
                // 在调用前固定不可重放边界，契约违约时保留代码但绝不再次 destroy。
                phase = InstancePhase::broken;
                auto binding = host.bind(); auto call = lifecycle->enter();
                call.api().destroy(instance);
                instance = nullptr;
                phase = InstancePhase::destroyed;
            }
            if (!host.finish_shutdown()) { error = DFY_V2_FAILED; return ShutdownResult::failed; }
            lifecycle.reset();
            const auto result = module.close();
            if (result != platform::ModuleClose::closed) return result == platform::ModuleClose::busy ? ShutdownResult::busy : ShutdownResult::in_use;
            phase = InstancePhase::unloaded;
            return ShutdownResult::closed;
        } catch (...) { error = DFY_V2_FAILED; return ShutdownResult::failed; }
    }
    PluginModuleV2 module;
    HostServices host;
    std::shared_ptr<const EffectiveConfig> config;
    std::optional<Capability<dfy_v2_lifecycle_api>> lifecycle;
    std::string id;
    dfy_v2_instance* instance = nullptr;
    InstancePhase phase = InstancePhase::loaded;
    uint32_t error = 0;
    bool create_attempted = false, start_attempted = false, stop_requested = false, stopped = false;
    State* next = nullptr;
};

// 先核对全局有界名额并读取配置，构造阶段不执行插件 create。
PluginInstance::PluginInstance(const Manifest& manifest, const std::filesystem::path& user_data_root,
    std::shared_ptr<events::BroadcastChannel> channel, InstanceLimits limits, std::shared_ptr<events::ConsumerGroup> group) {
    if (resident_ >= maximum_instances) throw std::length_error("instance: host state capacity exhausted");
    state_ = std::make_unique<State>(manifest, load_config(manifest, user_data_root), std::move(channel), limits, std::move(group));
}
// 未完成状态转入预留在对象内的链节点，不分配内存，也不丢失清理责任。
PluginInstance::~PluginInstance() {
    if (state_ && state_->shutdown() != ShutdownResult::closed) {
        state_->next = retained_;
        retained_ = state_.release();
    }
}
// 接受一次创建，失败指针违反契约时保留模块，不调用未知实例地址。
// 步骤：核对阶段 → 在保护内创建 → 固定成功或故障状态 → 对正常失败推进宿主清理。
uint32_t PluginInstance::create() noexcept {
    if (state_->create_attempted || state_->phase != InstancePhase::loaded) return DFY_V2_BUSY;
    state_->create_attempted = true;
    dfy_v2_instance* result = nullptr;
    const auto status = state_->invoke([&](const auto& api, auto* error) {
        return api.create(HostServices::query, {state_->config->json.data(), state_->config->json.size()}, &result, error);
    });
    if (!status && result) { state_->instance = result; state_->phase = InstancePhase::created; return DFY_V2_OK; }
    state_->error = status ? status : DFY_V2_FAILED;
    if (result) state_->phase = InstancePhase::broken;
    else (void)state_->shutdown();
    return state_->error;
}
// 启动失败立即关闭宿主入口，保留插件私有状态供停止与重试。
uint32_t PluginInstance::start() noexcept {
    if (state_->phase != InstancePhase::created || state_->start_attempted) return DFY_V2_BUSY;
    state_->start_attempted = true;
    const auto status = state_->invoke([&](const auto& api, auto* error) { return api.start(state_->instance, error); });
    if (!status) state_->phase = InstancePhase::active;
    else { state_->error = status; try { (void)state_->host.begin_shutdown(); } catch (...) {} }
    return status;
}
// 推进同一实例的关停记录，不制造第二个销毁流程。
ShutdownResult PluginInstance::shutdown() noexcept { return state_->shutdown(); }
// 返回当前阶段供宿主显示和验收。
InstancePhase PluginInstance::phase() const noexcept { return state_->phase; }
// 返回最近失败码，成功收尾不抹去历史故障。
uint32_t PluginInstance::last_error() const noexcept { return state_->error; }
// 为明确的外部代码依赖签发凭证。
platform::CodeLease PluginInstance::retain_code() { return state_->module.retain_code(); }
// 由宿主显式推进读取和通知，执行器运行由外层控制。
runtime::ScheduleResult PluginInstance::advance(runtime::FifoExecutor& executor) {
    state_->host.advance();
    return state_->host.schedule_one(executor);
}
// 复用实例宿主责任，将就绪通知送入公平调度及控制预留通路。
runtime::ScheduleResult PluginInstance::advance(runtime::OwnerScheduler& scheduler, runtime::Handle owner, runtime::TaskPriority priority) {
    state_->host.advance();
    return state_->host.schedule_one(scheduler,owner,priority);
}
// 复制保留项的非敏感诊断，原状态和代码继续留在宿主。
std::vector<RetainedInstance> PluginInstance::retained() {
    std::vector<RetainedInstance> result;
    for (auto* item = retained_; item; item = item->next) result.push_back({item->id, item->phase, item->error});
    return result;
}
// 逐项重试未完成状态，仅删除已经完成的节点；保留表本身不在进程退出时强制卸载。
// 步骤：拒绝递归并固定本轮边界 → 推进或重新保留节点 → 返回剩余责任数量。
std::size_t PluginInstance::retry_retained() {
    static bool retrying = false;
    if (retrying) throw std::logic_error("instance: recursive retained retry");
    retrying = true;
    auto* pending = std::exchange(retained_, nullptr);
    while (pending) {
        auto* item = pending;
        pending = item->next;
        if (item->shutdown() == ShutdownResult::closed) delete item;
        else { item->next = retained_; retained_ = item; }
    }
    retrying = false;
    std::size_t remaining = 0;
    for (auto* item = retained_; item; item = item->next) ++remaining;
    return remaining;
}
}
