#include "host.hpp"
#include "plugin_actions.hpp"
#include "plugin_directory.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace dfy::management {
namespace {
using Json = nlohmann::json;
constexpr std::size_t receipt_limit = 64;

// 写入一次没有分派的裁定并返回可脱离账本的值副本。
PluginOperation refused(PluginOperation op, std::string_view state) {
    op.state = state;
    op.effect = "notApplied";
    return op;
}

// 在原业务线程加载或卸载一个确切代次，依赖和活动保护沿用管理器的默认策略。
// 步骤：复核修订、包和实例代次 → 调用已有生命周期入口 → 记录直接返回及已核实效果。
void execute_plugin(plugins::PluginManager& manager, const PluginDirectory& view, PluginOperation& op) {
    const auto& cmd = op.command;
    const bool loading = cmd.action=="load";
    if ((!loading && cmd.action!="unload") || loading!=(cmd.generation==0)) { op = refused(std::move(op),"invalid"); return; }
    const auto found = view.packages.find(cmd.id);
    if (!view.available) { op = refused(std::move(op),"unavailable"); return; }
    if (cmd.revision!=view.revision || found==view.packages.end() || found->second.generation!=cmd.generation) {
        op = refused(std::move(op),"stale");
        return;
    }
    if (!loading) {
        if (!found->second.can_unload()) { op = refused(std::move(op),"busy"); return; }
        using Result = plugins::ShutdownResult;
        const auto result = manager.unload(cmd.id);
        switch (result) {
        case Result::closed: op.native_result="closed"; op.state="succeeded"; op.effect="unloaded"; break;
        case Result::pending: op.native_result=op.state="pending"; op.effect="stopping"; break;
        case Result::busy: op.native_result=op.state="busy"; op.effect="retained"; break;
        case Result::in_use: op.native_result=op.state="inUse"; op.effect="retained"; break;
        case Result::failed: op.native_result=op.state="failed"; op.effect="retained"; break;
        }
        return;
    }
    if (!found->second.can_load()) { op = refused(std::move(op),"busy"); return; }

    try { op.native_code = manager.load(cmd.id); }
    catch (const std::exception&) {
        // 依赖校验和模块打开失败可先于原生返回码，实效继续按管理器持有的代次核实。
    }
    if (op.native_code==DFY_V2_OK) {
        op.generation = manager.instance(cmd.id)->generation();
        op.state = "succeeded";
        op.effect = "loaded";
        return;
    }
    op.state = op.native_code==DFY_V2_BUSY ? "busy" : "failed";
    op.effect = "notLoaded";
    for (const auto& live : manager.loaded()) if (live.id==cmd.id) {
        op.generation = live.generation;
        op.effect = "retained";
    }
}

// 只发布原操作、程序裁定及已确认实效，数字身份保持十进制字符串。
Json operation_json(const PluginOperation& op, const Host& host, const Json& request) {
    Json result{{"version","1"},{"event","plugins.operation"},{"request",request.at("request")},{"instance",host.instance()},
        {"operation",op.identity},{"state",op.state},{"effect",op.effect}};
    if (op.command.number) result.update(Json{{"action",op.command.action},{"id",op.command.id},{"command",std::to_string(op.command.number)},
        {"revision",std::to_string(op.command.revision)},{"targetGeneration",std::to_string(op.command.generation)},
        {"generation",std::to_string(op.generation)}});
    if (op.expected) result["expectedCommand"] = std::to_string(op.expected);
    if (op.native_code) result["nativeCode"] = *op.native_code;
    if (!op.native_result.empty()) result["nativeResult"] = op.native_result;
    return result;
}
}

// 按当前窗口的连续命令号接纳意图，跨窗口恢复通过原操作号读取。
// 步骤：核验客户端与后台 → 防重并核对连续序号 → 回收有界终态或保留原工作。
PluginOperation Host::submit_plugin(const std::string& client, const std::string& instance, PluginCommand cmd) {
    std::lock_guard lock(mutex_);
    const auto owner = clients_.find(client);
    if (client.empty() || owner==clients_.end()) throw std::invalid_argument("plugins: management client required");
    PluginOperation op{"plugin/"+client+"/"+std::to_string(cmd.number),std::move(cmd)};
    op.generation = op.command.generation;
    if (instance!=instance_) return refused(std::move(op),"stale");
    for (const auto& prior : plugin_operations_) if (prior->identity==op.identity)
        return prior->command==op.command ? *prior : refused(std::move(op),"conflict");
    auto& latest = owner->second.plugin_latest;
    if (op.command.number<=latest) { op.state="expired"; op.effect="unknown"; return op; }
    if (op.command.number!=latest+1) { op.expected=latest+1; return refused(std::move(op),"outOfOrder"); }
    if (exited_ || worker_.get_stop_token().stop_requested()) return refused(std::move(op),"unavailable");
    if (plugin_operations_.size()==receipt_limit) {
        const auto retired = std::ranges::find_if(plugin_operations_,[](const auto& item) { return !item->unfinished(); });
        if (retired==plugin_operations_.end()) { latest=op.command.number; return refused(std::move(op),"full"); }
        plugin_operations_.erase(retired);
    }
    plugin_operations_.push_back(std::make_shared<PluginOperation>(op));
    latest = op.command.number;
    ++queued_plugins_;
    return op;
}

// 允许同一后台已握手的管理窗口沿原操作号只读复查，失效后台不接入当前记录。
PluginOperation Host::plugin_receipt(const std::string& client, const std::string& instance, const std::string& identity) const {
    std::lock_guard lock(mutex_);
    if (client.empty() || !clients_.contains(client)) throw std::invalid_argument("plugins: management client required");
    if (instance==instance_)
        for (const auto& record : plugin_operations_) if (record->identity==identity) return *record;
    return {identity,{},instance==instance_ ? "unknown" : "stale","unknown"};
}

// 持有宿主锁时按原代次结算已受理的停止，真实退出与拒绝均保留最初的管理器返回值。
// 步骤：发布本轮目录 → 定位待退出原代次 → 依据已消失或失败保留的事实结算。
void Host::publish_plugin_directory(std::shared_ptr<const PluginDirectory> view) {
    packages_ = std::move(view);
    if (!packages_ || !waiting_plugins_) return;
    for (auto& record : plugin_operations_) {
        if (record->state!="pending") continue;
        const auto found = packages_->packages.find(record->command.id);
        if (found==packages_->packages.end() || found->second.generation!=record->command.generation) {
            record->state="succeeded"; record->effect="unloaded";
        } else if (found->second.state=="failed") {
            record->state="failed"; record->effect="retained";
        } else continue;
        --waiting_plugins_;
    }
}

// 在业务线程按最新目录执行一个原操作，并将实效与新目录一起发布。
// 步骤：采样现场 → 移交一项已接纳工作 → 复核并执行 → 使用同一锁结算回执和目录。
void Host::advance_plugin_actions(plugins::PluginManager& manager, std::shared_ptr<const PluginDirectory>& previous) {
    std::shared_ptr<const PluginDirectory> view;
    try { previous = sample_plugins(manager,previous); view = previous; } catch (const std::exception&) {}
    std::shared_ptr<PluginOperation> job;
    {
        std::lock_guard lock(mutex_);
        if (!queued_plugins_) { publish_plugin_directory(std::move(view)); return; }
        const auto next = std::ranges::find_if(plugin_operations_,[](const auto& item) { return item->state=="queued"; });
        job = *next;
        job->state = "running";
        --queued_plugins_;
    }
    auto result = *job;
    try {
        if (view) execute_plugin(manager,*view,result);
        else result = refused(std::move(result),"unavailable");
    } catch (const std::exception&) { result.state="failed"; result.effect="unconfirmed"; }
    try { previous = sample_plugins(manager,previous); view = previous; } catch (const std::exception&) { view.reset(); }
    std::lock_guard lock(mutex_);
    *job = std::move(result);
    if (job->state=="pending") ++waiting_plugins_;
    publish_plugin_directory(std::move(view));
}

// 只接受精确的包 ID、代次、目录修订和命令号，包的磁盘路径由管理器定位。
// 步骤：解析有界身份 → 区分查询或受控写入 → 回显原请求及宿主回执。
Json plugin_action_request(Host& host, const Json& request, const std::string& client) {
    const auto& instance = plugin_identity(request.at("instance"),64);
    const auto& type = request.at("type");
    if (type=="plugins.receipt" && request.size()==5)
        return operation_json(host.plugin_receipt(client,instance,plugin_identity(request.at("operation"),160)),host,request);
    if ((type!="plugins.load" && type!="plugins.unload") || request.size()!=8) throw std::invalid_argument("plugins: invalid operation");
    PluginCommand cmd{type=="plugins.load" ? "load" : "unload",plugin_identity(request.at("id"),64),management_number(request.at("command")),
        management_number(request.at("revision")),management_number(request.at("generation"))};
    if (!cmd.number || (cmd.action=="load")!=(cmd.generation==0)) throw std::invalid_argument("plugins: invalid command or target generation");
    return operation_json(host.submit_plugin(client,instance,std::move(cmd)),host,request);
}
}
