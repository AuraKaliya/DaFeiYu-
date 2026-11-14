#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_package.hpp>
#include "package_files.hpp"

namespace dfy::plugins {
namespace {
// 在任何按容量分配前限定实际实例上限，依赖目录最大为 64 的平方。
std::size_t bounded(std::size_t capacity) {
    if (!capacity || capacity > PluginInstance::maximum_instances) throw std::invalid_argument("plugins: capacity must be 1..64");
    return capacity;
}
}
struct PluginManager::State {
    struct Entry {
        std::shared_ptr<PluginInstance> instance; runtime::Handle owner;
        bool stopping = false, owner_stopped = false, faulted = false; DependencyStop stop_mode = DependencyStop::refuse_dependents;
    };
    // 固定目录基准和全部运行时容量，扫描不执行插件入口。
    State(const std::filesystem::path& package_root, const std::filesystem::path& data,
        std::shared_ptr<events::BroadcastChannel> events, std::size_t limit)
        : registry(package_root), root(std::filesystem::absolute(package_root).lexically_normal()),
          user_data(std::filesystem::absolute(data).lexically_normal()), channel(std::move(events)), scheduler(bounded(limit)),
          dependencies(bounded(limit)*bounded(limit)), capacity(limit) {
        if (!channel || data.empty()) throw std::invalid_argument("plugins: channel and user data path required");
        try { registry.refresh(); } catch (const std::runtime_error& error) { directory_error = error.what(); }
    }
    // 仅回收已卸载实例及其已排空调度名额，排队宿主包装仍保留到实际执行或撤销后。
    // 步骤：筛选已卸载实例 → 停止其调度接纳 → 等待包装排空 → 归还名额与实例持有。
    void collect() {
        for (auto item = instances.begin(); item != instances.end();) {
            auto& entry = item->second;
            if (entry.instance->phase() != InstancePhase::unloaded) { ++item; continue; }
            if (!entry.owner_stopped) { scheduler.stop(entry.owner); entry.owner_stopped = true; }
            if (scheduler.size(entry.owner)) { ++item; continue; }
            scheduler.remove(entry.owner); item = instances.erase(item);
        }
    }
    // 正常停止等待继续轮询，生命周期失败保留给显式重试，不自动反复调用失败入口。
    // 步骤：推进停止或通知 → 执行一个所有者步骤 → 回收已退出实例。
    runtime::RunResult advance() {
        bool failed = false;
        for (auto& [id,entry] : instances) {
            try {
                if (entry.stopping) entry.stopping = dependencies.shutdown(entry.instance,entry.stop_mode).result == ShutdownResult::pending;
                else if (!entry.faulted && entry.instance->phase() == InstancePhase::active) entry.instance->advance(scheduler,entry.owner);
            } catch (...) { entry.faulted = true; entry.stopping = false; failed = true; }
        }
        const auto result = scheduler.run_one(); collect(); return failed ? runtime::RunResult::failed : result;
    }
    // 有序停止全部实例，失败保持整个管理状态，不能依靠字段析构释放仍在使用的代码。
    // 步骤：关闭加载接纳 → 联动停止具体实例 → 排空已有宿主包装 → 回收或保留责任。
    bool close() noexcept {
        accepting = false;
        try {
            for (auto& [id,entry] : instances) {
                const auto stopped = dependencies.shutdown(entry.instance,DependencyStop::linked);
                entry.stopping = stopped.result == ShutdownResult::pending;
                entry.stop_mode = DependencyStop::linked;
            }
            while (scheduler.run_one() != runtime::RunResult::idle) {}
            collect(); return instances.empty();
        } catch (...) { return false; }
    }
    Registry registry;
    std::filesystem::path root, user_data;
    std::shared_ptr<events::BroadcastChannel> channel;
    runtime::OwnerScheduler scheduler;
    PluginDependencies dependencies;
    const std::size_t capacity;
    std::map<std::string,Entry> instances;
    std::string directory_error;
    bool accepting = true;
    State* next = nullptr;
};
PluginManager::State* PluginManager::retained_ = nullptr;
// 完整创建管理状态后发布控制器，失败不留下局部实例。
PluginManager::PluginManager(const std::filesystem::path& root, const std::filesystem::path& user_data,
    std::shared_ptr<events::BroadcastChannel> channel, std::size_t capacity)
    : state_(std::make_unique<State>(root,user_data,std::move(channel),capacity)) {}
// 失败保留链节点已在状态内预留，不在退出时申请新内存。
// 步骤：尝试完整停止 → 将未完成状态转交保留链并保留全部运行责任。
PluginManager::~PluginManager() {
    if (!state_->close()) { state_->next = retained_; retained_ = state_.release(); }
}
// 刷新描述快照，不根据变化自动替换已有实例。
// 步骤：尝试替换目录快照 → 成功清除旧错误或保存独立失败 → 按结果返回。
void PluginManager::refresh() {
    try { state_->registry.refresh(); state_->directory_error.clear(); }
    catch (const std::runtime_error& error) { state_->directory_error = error.what(); throw; }
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
// 步骤：校验目录及加载接纳 → 预留管理记录和调度者 → 装配实例或回滚 → 创建启动并处理失败。
std::uint32_t PluginManager::load(const std::string& id) {
    state_->collect();
    if (!state_->accepting || state_->instances.contains(id)) return DFY_V2_BUSY;
    const auto snapshot = catalog(); const auto found = snapshot->packages.find(id);
    if (found == snapshot->packages.end()) throw std::invalid_argument("plugins: unknown or unavailable id");
    if (state_->instances.size() == state_->capacity) return DFY_V2_RESOURCE_EXHAUSTED;
    auto item = state_->instances.try_emplace(id).first;
    try {
        item->second.owner = state_->scheduler.add_owner(1,8,2);
        item->second.instance = std::make_shared<PluginInstance>(found->second,state_->user_data,state_->channel);
    } catch (...) {
        if (!item->second.owner.empty()) { state_->scheduler.stop(item->second.owner); state_->scheduler.remove(item->second.owner); }
        state_->instances.erase(item); throw;
    }
    auto status = item->second.instance->create();
    if (!status) status = item->second.instance->start();
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
    item->second.stopping = result == ShutdownResult::pending; item->second.stop_mode = mode; state_->collect(); return result;
}
// 将宿主一次推进转交给管理状态，保留单步公平调度。
runtime::RunResult PluginManager::advance() { return state_->advance(); }
// 显式停止全部管理责任，调用方可在 Pending 后继续推进再重试。
bool PluginManager::close() noexcept { return state_->close(); }
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
