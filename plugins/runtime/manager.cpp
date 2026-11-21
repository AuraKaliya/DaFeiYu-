#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_package.hpp>
#include <dafeiyu/plugin_order.hpp>
#include <dafeiyu/digest.hpp>
#include "package_files.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <nlohmann/json.hpp>

namespace dfy::plugins {
namespace {
// 在任何按容量分配前限定实际实例上限，依赖目录最大为 64 的平方。
std::size_t bounded(std::size_t capacity) {
    if (!capacity || capacity > PluginInstance::maximum_instances) throw std::invalid_argument("plugins: capacity must be 1..64");
    return capacity;
}

// 计算单次调用与原插件实例之间的会话范围。
uint32_t hook_scope(uint64_t owner, const PluginInstance& instance) {
    return !owner ? DFY_V2_HOOK_MAIN_SESSION : owner == instance.generation()
        ? DFY_V2_HOOK_OWN_SESSION : DFY_V2_HOOK_OTHER_SESSION;
}

// 在装载 DLL 前检查所选包的传递依赖，其他包的声明保持独立。
// 步骤：收集依赖闭包 → 构建完整有向图 → 检查缺失及环路。
void validate_dependencies(const Catalog& catalog, const Manifest& selected) {
    std::vector<const Manifest*> closure{&selected};
    for (std::size_t index = 0; index < closure.size(); ++index) {
        for (const auto& id : closure[index]->dependencies) {
            const auto found = catalog.packages.find(id);
            if (found == catalog.packages.end()) throw std::invalid_argument("missing plugin dependency: "+id);
            if (std::ranges::find(closure,&found->second) == closure.end()) closure.push_back(&found->second);
        }
    }

    std::vector<std::vector<std::string_view>> edges;
    std::vector<PluginOrderNode> graph;
    edges.reserve(closure.size());
    graph.reserve(closure.size());
    for (const auto* item : closure) {
        auto& providers = edges.emplace_back(item->dependencies.begin(),item->dependencies.end());
        graph.push_back({item->id,providers});
    }
    (void)stable_plugin_order(graph);
}
}
struct PluginManager::State {
    struct Entry {
        std::shared_ptr<PluginInstance> instance; runtime::Handle owner;
        std::uint64_t registration = 0;
        bool stopping = false, owner_stopped = false, faulted = false; DependencyStop stop_mode = DependencyStop::refuse_dependents;
        std::chrono::steady_clock::time_point next_activity{};
        bool activity_queued = false;
    };
    struct Hook {
        std::shared_ptr<PluginInstance> instance;
        std::size_t index;
        uint32_t sessions;
        bool faulted;
    };
    using HookList = std::vector<Hook>;
    using PluginSet = std::bitset<PluginInstance::maximum_instances>;
    struct TextHook {
        Hook hook;
        std::size_t node;
        bool chain;
    };
    struct HookPlan {
        std::array<HookList,3> controls;
        std::array<std::vector<TextHook>,DFY_V2_TEXT_HOOK_REPLY> rewrites;
        std::array<PluginSet,PluginInstance::maximum_instances> ancestors;
        std::string version = "none";
    };
    // 固定目录基准和全部运行时容量，扫描不执行插件入口。
    State(const std::filesystem::path& package_root, const std::filesystem::path& data,
        std::shared_ptr<events::BroadcastChannel> events, std::size_t limit, PluginMailboxHost mail)
        : registry(package_root), root(std::filesystem::absolute(package_root).lexically_normal()),
          user_data(std::filesystem::absolute(data).lexically_normal()), channel(std::move(events)), scheduler(bounded(limit)),
          dependencies(bounded(limit)*bounded(limit)), mailboxes(std::move(mail)), capacity(limit) {
        if (!channel || data.empty()) throw std::invalid_argument("plugins: channel and user data path required");
        if (static_cast<bool>(mailboxes.network) != (mailboxes.identity != 0))
            throw std::invalid_argument("plugins: mail network and host identity must be supplied together");
        try { registry.refresh(); } catch (const std::runtime_error& error) { directory_error = error.what(); }
    }
    // 发布按控制点索引的不可变调用表，依赖排序覆盖没有注册 Hook 的节点。
    // 步骤：按注册顺序建立活动节点 → 复制完整边并排序 → 分派各索引到其控制点 → 原子替换。
    void publish_hooks() {
        std::vector<const Entry*> active;
        for (const auto& [id, entry] : instances)
            if (entry.instance && entry.instance->phase() == InstancePhase::active) active.push_back(&entry);
        std::ranges::sort(active,{},[](const auto* entry) { return entry->registration; });

        std::vector<std::vector<std::string_view>> edges;
        std::vector<PluginOrderNode> graph;
        edges.reserve(active.size());
        graph.reserve(active.size());
        for (const auto* entry : active) {
            auto& providers = edges.emplace_back(dependencies.providers(*entry->instance));
            std::erase_if(providers,[&](const auto id) { return !std::ranges::any_of(active,[&](const auto* item) {
                return item->instance->plugin_id() == id;
            }); });
            graph.push_back({entry->instance->plugin_id(),providers});
        }

        auto updated = std::make_shared<HookPlan>();
        auto registration = nlohmann::json::array();
        for (const auto node : stable_plugin_order(graph)) {
            const auto& instance = active[node]->instance;
            auto& ancestry = updated->ancestors[node];
            ancestry.set(node);
            for (const auto provider : edges[node]) {
                const auto found = std::ranges::find(graph,provider,&PluginOrderNode::id);
                ancestry |= updated->ancestors[static_cast<std::size_t>(found-graph.begin())];
            }
            const auto registered = instance->hooks();
            auto controls = nlohmann::json::array();
            for (std::size_t index = 0; index < registered.size(); ++index) {
                const auto& hook = registered[index];
                updated->controls.at(hook.point-1).push_back({instance,index,hook.sessions,active[node]->faulted});
                controls.push_back({hook.point,hook.sessions});
            }
            const auto rewrites = instance->text_hooks();
            auto fields = nlohmann::json::array();
            for (std::size_t index = 0; index < rewrites.size(); ++index) {
                const auto& hook = rewrites[index];
                updated->rewrites.at(hook.point-1).push_back({{instance,index,hook.sessions,active[node]->faulted},node,
                    (hook.flags & DFY_V2_TEXT_HOOK_CHAIN) != 0});
                fields.push_back({hook.point,hook.sessions,hook.flags});
            }
            if (controls.empty() && fields.empty()) continue;
            auto providers = nlohmann::json::array();
            for (std::size_t i = 0; i < graph.size(); ++i) if (i != node && ancestry.test(i)) providers.push_back(graph[i].id);
            registration.push_back({instance->plugin_id(),instance->plugin_version(),active[node]->faulted,
                std::move(providers),std::move(controls),std::move(fields)});
        }
        if (!registration.empty()) updated->version = "hooks-v1:sha256:"+platform::sha256(registration.dump());
        hook_order = std::move(updated);
        hook_revision = dependencies.revision();
        hooks_dirty = false;
    }
    // 同一宿主线程按依赖或实例状态变化更新固定快照。
    std::shared_ptr<const HookPlan> hook_plan() {
        if (hooks_dirty || hook_revision != dependencies.revision()) publish_hooks();
        return hook_order;
    }
    // 仅回收已卸载实例及其已排空调度名额，排队宿主包装仍保留到实际执行或撤销后。
    // 步骤：筛选已卸载实例 → 停止其调度接纳 → 等待包装排空 → 归还名额与实例持有。
    void collect() {
        for (auto item = instances.begin(); item != instances.end();) {
            auto& entry = item->second;
            if (entry.instance->phase() != InstancePhase::unloaded) { ++item; continue; }
            if (!entry.owner_stopped) { scheduler.stop(entry.owner); entry.owner_stopped = true; }
            if (scheduler.size(entry.owner)) { ++item; continue; }
            scheduler.remove(entry.owner); item = instances.erase(item); hooks_dirty = true;
        }
    }
    // 每实例至多排队一步，复用插件通知的所有者公平通路。
    // 步骤：核对运行资格与节拍 → 排入一步 → 派发时复核实例并更新下次时间。
    void schedule_activity(Entry& entry) {
        if (!accepting || entry.activity_queued || !entry.instance->has_activity() ||
            entry.next_activity > std::chrono::steady_clock::now() || !scheduler.available(entry.owner,runtime::TaskPriority::low)) return;
        scheduler.post(entry.owner,[this,&entry] {
            entry.activity_queued = false;
            if (!accepting || entry.faulted || !entry.instance->has_activity()) return;
            try {
                const auto wait = entry.instance->advance_activity();
                entry.next_activity = std::chrono::steady_clock::now()+std::chrono::milliseconds(wait);
            } catch (...) { entry.faulted = true; hooks_dirty = true; throw; }
        },{},runtime::TaskPriority::low);
        entry.activity_queued = true;
    }
    // 正常停止等待继续轮询，生命周期失败保留给显式重试，不自动反复调用失败入口。
    // 步骤：推进停止或通知 → 执行一个所有者步骤 → 回收已退出实例。
    runtime::RunResult advance() {
        bool failed = false;
        for (auto& [id,entry] : instances) {
            try {
                if (entry.stopping) {
                    const auto result = dependencies.shutdown(entry.instance,entry.stop_mode).result;
                    entry.stopping = result == ShutdownResult::pending;
                    if (result == ShutdownResult::failed) { failed = true; if (!accepting) close_result = result; }
                }
                else if (!entry.faulted && entry.instance->phase() == InstancePhase::active) {
                    entry.instance->advance(scheduler,entry.owner);
                    schedule_activity(entry);
                }
            } catch (...) { entry.faulted = true; entry.stopping = false; hooks_dirty = true; failed = true; if (!accepting) close_result = ShutdownResult::failed; }
        }
        const auto result = scheduler.run_one(); collect(); return failed ? runtime::RunResult::failed : result;
    }
    // 有序停止全部实例，失败保持整个管理状态，不能依靠字段析构释放仍在使用的代码。
    // 步骤：关闭加载接纳 → 冻结去重依赖计划并各尝试一次 → 记录本轮结果 → 排空包装并回收或保留责任。
    bool close() noexcept {
        accepting = false; close_result = ShutdownResult::closed;
        try {
            std::vector<std::shared_ptr<PluginInstance>> roots; roots.reserve(instances.size());
            for (const auto& [id,entry] : instances) roots.push_back(entry.instance);
            for (const auto& stopped : dependencies.shutdown_all(roots)) {
                if (stopped.result == ShutdownResult::failed || (stopped.result != ShutdownResult::closed && close_result == ShutdownResult::closed))
                    close_result = stopped.result;
                for (auto& [id,entry] : instances) if (entry.instance == stopped.instance) {
                    entry.stopping = stopped.result == ShutdownResult::pending;
                    entry.stop_mode = DependencyStop::refuse_dependents;
                }
            }
            while (scheduler.run_one() != runtime::RunResult::idle) {}
            collect(); return instances.empty();
        } catch (...) { close_result = ShutdownResult::failed; return false; }
    }
    Registry registry;
    std::filesystem::path root, user_data;
    std::shared_ptr<events::BroadcastChannel> channel;
    runtime::OwnerScheduler scheduler;
    PluginDependencies dependencies;
    PluginMailboxHost mailboxes;
    std::weak_ptr<PluginAgents> agents;
    const std::size_t capacity;
    std::map<std::string,Entry> instances;
    std::string event_cursor;
    std::shared_ptr<const HookPlan> hook_order;
    std::uint64_t registration = 0, hook_revision = 0;
    std::string directory_error;
    EventCapture event_capture;
    bool accepting = true, hooks_dirty = false;
    ShutdownResult close_result = ShutdownResult::closed;
    State* next = nullptr;
};
PluginManager::State* PluginManager::retained_ = nullptr;
// 完整创建管理状态后发布控制器，失败不留下局部实例。
PluginManager::PluginManager(const std::filesystem::path& root, const std::filesystem::path& user_data,
    std::shared_ptr<events::BroadcastChannel> channel, std::size_t capacity, PluginMailboxHost mailboxes)
    : state_(std::make_unique<State>(root,user_data,std::move(channel),capacity,std::move(mailboxes))) {}
// 失败保留链节点已在状态内预留，不在退出时申请新内存。
// 步骤：保留已报告失败或尝试停止 → 将未完成状态转交保留链，不隐式重试已知失败。
PluginManager::~PluginManager() {
    const bool failed = !state_->accepting && state_->close_result == ShutdownResult::failed;
    if (failed || !state_->close()) { state_->next = retained_; retained_ = state_.release(); }
}
// 刷新描述快照，不根据变化自动替换已有实例。
// 步骤：尝试替换目录快照 → 成功清除旧错误或保存独立失败 → 按结果返回。
void PluginManager::refresh() {
    try { state_->registry.refresh(); state_->directory_error.clear(); }
    catch (const std::runtime_error& error) { state_->directory_error = error.what(); throw; }
}
// 在任何实例创建前固定可选子会话服务。
void PluginManager::enable_agents(const std::shared_ptr<PluginAgents>& service) {
    if (!service || !state_->accepting || state_->registration || !state_->instances.empty() || !state_->agents.expired())
        throw std::logic_error("plugins: cannot attach agent service");
    state_->agents = service;
}
// 固定应用提供的统一事件接纳记录器。
void PluginManager::capture_events(EventCapture capture) {
    if (!capture || !state_->accepting || !state_->instances.empty() || state_->event_capture)
        throw std::logic_error("plugins: cannot attach event capture");
    state_->event_capture = std::move(capture);
}
// 返回当前描述目录的独立共享快照。
std::shared_ptr<const Catalog> PluginManager::catalog() const { return state_->registry.snapshot(); }
// 目录失败作为插件诊断保留，本体模型调用不依赖目录扫描成功。
std::string_view PluginManager::directory_error() const noexcept { return state_->directory_error; }
// 复制非敏感实例状态供终端展示，不暴露配置或凭据。
std::vector<LoadedPlugin> PluginManager::loaded() const {
    std::vector<LoadedPlugin> result;
    for (const auto& [id,entry] : state_->instances) result.push_back({id,entry.instance->phase(),entry.faulted ? DFY_V2_FAILED : entry.instance->last_error()});
    return result;
}
// 为实例预留目录与调度名额，再执行一次创建及启动，失败沿既有生命周期回收。
// 步骤：检查所选包及依赖 → 装配并启动实例 → 绑定提供方及发布 Hook 顺序 → 失败时有序回收。
std::uint32_t PluginManager::load(const std::string& id) {
    state_->collect();
    if (!state_->accepting || state_->instances.contains(id)) return DFY_V2_BUSY;
    const auto snapshot = catalog(); const auto found = snapshot->packages.find(id);
    if (found == snapshot->packages.end()) throw std::invalid_argument("plugins: unknown or unavailable id");
    if (state_->instances.size() == state_->capacity) return DFY_V2_RESOURCE_EXHAUSTED;
    const auto& required = found->second.dependencies;
    if (!required.empty()) validate_dependencies(*snapshot,found->second);
    for (const auto& provider : required) {
        const auto dep = state_->instances.find(provider);
        if (dep == state_->instances.end() || dep->second.instance->phase() != InstancePhase::active || dep->second.faulted)
            throw std::logic_error("plugin dependency must be active: "+provider);
    }
    auto item = state_->instances.try_emplace(id).first;
    try {
        item->second.owner = state_->scheduler.add_owner(1,8,2);
        item->second.instance = std::make_shared<PluginInstance>(found->second,state_->user_data,state_->channel);
        if (state_->event_capture) item->second.instance->capture_events(state_->event_capture);
        if (state_->mailboxes.network)
            item->second.instance->enable_mailboxes(state_->mailboxes.network,state_->mailboxes.identity);
        if (!state_->agents.expired()) item->second.instance->enable_agents(state_->agents,item->second.instance);
    } catch (...) {
        if (!item->second.owner.empty()) { state_->scheduler.stop(item->second.owner); state_->scheduler.remove(item->second.owner); }
        state_->instances.erase(item); throw;
    }
    auto status = item->second.instance->create();
    if (!status) status = item->second.instance->start();
    if (!status) {
        item->second.registration = ++state_->registration;
        try {
            for (const auto& provider : required)
                (void)state_->dependencies.bind(item->second.instance,state_->instances.at(provider).instance);
            state_->publish_hooks();
        }
        catch (...) {
            if (const auto agents = state_->agents.lock()) agents->close_owner(item->second.instance->generation());
            status = DFY_V2_FAILED;
        }
    }
    if (status) (void)unload(id);
    return status;
}
// 先确认同 ID 没有运行责任，再安装并独立刷新描述，已发布包不因刷新失败被回滚。
// 步骤：核对管理接纳及实例 → 完整安装 → 刷新目录或保留独立诊断 → 返回发布事实。
Manifest PluginManager::install(const std::filesystem::path& source) {
    if (!state_->accepting) throw std::logic_error("plugins: manager closing");
    const auto descriptor = load_manifest(files::directory(source));
    if (state_->instances.contains(descriptor.id)) throw std::logic_error("plugins: finish unloading the existing instance before installing");
    auto installed = install_package(state_->root,source);
    // 发布已经完成，刷新错误由目录诊断保存，不把复制成功报告成安装回滚。
    try { refresh(); } catch (const std::runtime_error&) {}
    return installed;
}
// 已加载实例先按默认依赖规则退出，再移出包发现目录，用户设置不参与文件操作。
// 步骤：核对管理接纳 → 按需卸载并保留拒绝 → 移除或重试私有残留 → 刷新独立目录状态。
ManagedRemoval PluginManager::remove(const std::string& id) {
    if (!state_->accepting) throw std::logic_error("plugins: manager closing");
    if (state_->instances.contains(id)) {
        const auto result = unload(id);
        if (result != ShutdownResult::closed) return {result,{}};
    }
    auto result = remove_package(state_->root,id);
    if (!result.found) throw std::invalid_argument("plugins: package not installed and no removal is pending");
    // 包已退出发现目录，后续刷新错误独立保存，不把剩余清理误报为未发生移除。
    try { refresh(); } catch (const std::runtime_error&) {}
    return {ShutdownResult::closed,std::move(result)};
}
// 只在实例实际卸载后停止对应调度接纳，忙碌拒绝不改变活动实例。
// 步骤：定位具体实例 → 按依赖协议停止 → 保存继续等待方式 → 回收已结束名额。
ShutdownResult PluginManager::unload(const std::string& id, DependencyStop mode) {
    const auto item = state_->instances.find(id);
    if (item == state_->instances.end()) throw std::invalid_argument("plugins: id is not loaded");
    const auto result = state_->dependencies.shutdown(item->second.instance,mode).result;
    item->second.stopping = result == ShutdownResult::pending; item->second.stop_mode = mode; state_->collect();
    if (result == ShutdownResult::pending) state_->hooks_dirty = true;
    return result;
}
// 将宿主一次推进转交给管理状态，保留单步公平调度。
runtime::RunResult PluginManager::advance() { return state_->advance(); }
// 按实例轮转取出一件已解析事件，保持单实例接纳顺序。
// 步骤：从上一来源之后起步 → 检查活动实例 → 移动游标并交付结果。
std::optional<RoutedEvent> PluginManager::take_routed_event() {
    const auto& entries = state_->instances;
    auto item = entries.upper_bound(state_->event_cursor);
    for (std::size_t index = 0; index < entries.size(); ++index,++item) {
        if (item == entries.end()) item = entries.begin();
        if (item->second.faulted || item->second.stopping) continue;
        if (auto event = item->second.instance->take_routed_event()) {
            state_->event_cursor = item->first;
            return event;
        }
    }
    return {};
}
// 在同一份注册快照上依次调用适用实例，拒绝立即停止，确认条件累计到本步。
// 步骤：检查调用点 → 取得或刷新固定计划 → 按会话过滤及裁定。
uint32_t PluginManager::control_hook(const dfy_v2_hook_input& input) {
    if (!input.session || !input.run || input.reserved ||
        input.point < DFY_V2_HOOK_MODEL_REQUEST || input.point > DFY_V2_HOOK_TOOL_RESULT)
        throw std::invalid_argument("hooks: invalid execution context");
    const auto snapshot = state_->hook_plan();
    if (!snapshot) return DFY_V2_HOOK_ALLOW;

    auto result = DFY_V2_HOOK_ALLOW;
    for (const auto& hook : snapshot->controls.at(input.point-1)) {
        if (hook.instance->phase() != InstancePhase::active) continue;
        if (!(hook_scope(input.owner,*hook.instance) & hook.sessions)) continue;
        if (hook.faulted) throw std::runtime_error("hooks: registered controller is faulted");
        const auto verdict = hook.instance->decide_hook(hook.index,input);
        if (verdict == DFY_V2_HOOK_DENY) return verdict;
        if (verdict == DFY_V2_HOOK_CONFIRM) result = verdict;
    }
    return result;
}
// 只合并可归因于当前字段的提案，独立改写发生冲突时拒绝整个字段。
// 步骤：固定调用快照 → 按作用域选择处理器 → 检查接续依赖并裁定各提案。
std::optional<std::string> PluginManager::rewrite_hook(const dfy_v2_text_hook_input& input, std::size_t limit) {
    if (!input.session || !input.run || input.reserved || !input.field.data || !input.field.length || input.field.length > 256 ||
        (input.value.length && !input.value.data) || !limit || limit > 1024*1024 ||
        input.point < DFY_V2_TEXT_HOOK_INPUT || input.point > DFY_V2_TEXT_HOOK_REPLY)
        throw std::invalid_argument("text hooks: invalid context or limit");
    const auto snapshot = state_->hook_plan();
    if (!snapshot) return std::nullopt;

    State::PluginSet producers;
    std::optional<std::string> value;
    std::vector<char> output;
    for (const auto& item : snapshot->rewrites.at(input.point-1)) {
        const auto& hook = item.hook;
        if (hook.instance->phase() != InstancePhase::active || !(hook_scope(input.owner,*hook.instance) & hook.sessions)) continue;
        if (hook.faulted) throw std::runtime_error("text hooks: registered converter is faulted");
        auto candidate = input;
        const bool follows = value && item.chain && (producers & ~snapshot->ancestors[item.node]).none();
        if (follows) candidate.value = {value->data(),value->size()};
        if (output.empty()) output.resize(limit);
        auto replacement = hook.instance->rewrite_hook(hook.index,candidate,output);
        if (!replacement || *replacement == std::string_view(candidate.value.data ? candidate.value.data : "",candidate.value.length)) continue;
        if (value && !follows && *replacement != *value) throw std::runtime_error("text hooks: conflicting proposals for field "+
            std::string(input.field.data,input.field.length));
        producers.set(item.node);
        value = std::move(replacement);
    }
    return value;
}
// 把当前可复现的登记版本用于运行接纳和离线比较。
std::string PluginManager::hook_version() {
    const auto snapshot = state_->hook_plan();
    return snapshot ? snapshot->version : "none";
}
// 显式停止全部管理责任，调用方可在 Pending 后继续推进再重试。
bool PluginManager::close() noexcept { return state_->close(); }
// 只报告本轮批量关闭的裁定，历史生命周期错误继续留在实例诊断中。
ShutdownResult PluginManager::close_status() const noexcept { return state_->close_result; }
// 返回固定包管理根，文件系统操作不能仅依靠该字符串判断安全性。
const std::filesystem::path& PluginManager::root() const { return state_->root; }
// 取得当前具体实例，不因目录刷新切换到新对象。
std::shared_ptr<PluginInstance> PluginManager::instance(const std::string& id) const {
    const auto item = state_->instances.find(id);
    if (item == state_->instances.end()) throw std::invalid_argument("plugins: id is not loaded");
    return item->second.instance;
}
// 提供宿主显式运行时绑定入口，关系不写回包或配置。
PluginDependencies& PluginManager::dependencies() { return state_->dependencies; }
// 统计仍需实际回收的管理器，不把其丢弃或强制关闭。
std::size_t PluginManager::retained() noexcept { std::size_t count = 0; for (auto* item = retained_; item; item = item->next) ++count; return count; }
// 重试保留状态，先摘下本轮链防止新保留节点混入当前遍历。
// 步骤：拒绝递归 → 固定本轮链 → 关闭成功者或重新保留 → 返回剩余计量。
std::size_t PluginManager::retry_retained() {
    static bool retrying = false;
    if (retrying) throw std::logic_error("plugins: recursive retained retry");
    retrying = true; auto* pending = std::exchange(retained_,nullptr);
    while (pending) {
        auto* item = pending; pending = item->next;
        if (item->close()) delete item;
        else { item->next = retained_; retained_ = item; }
    }
    retrying = false; return retained();
}
}
