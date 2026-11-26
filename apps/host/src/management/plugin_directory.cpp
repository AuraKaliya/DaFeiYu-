#include "plugin_directory.hpp"
#include "host.hpp"
#include <charconv>
#include <nlohmann/json.hpp>

namespace dfy::management {
namespace {
using Json = nlohmann::json;
// 将宿主持有的实际生命周期映射为管理目录可展示的状态。
std::string_view package_state(const plugins::LoadedPlugin& live) {
    using Phase = plugins::InstancePhase;
    if (live.stop_pending || live.phase==Phase::unloaded) return "stopping";
    return live.phase==Phase::active && !live.error ? "active" : "failed";
}

// 只编码清单公开元数据和运行事实，配置及宿主路径保留在业务线程。
Json package_json(const std::string& id, const PackageRow& row) {
    Json result{{"id",id},{"name",row.name},{"version",row.version},{"state",row.state},{"installed",row.installed},
        {"loaded",row.generation!=0},{"generation",std::to_string(row.generation)},{"canLoad",row.can_load()},{"canUnload",row.can_unload()}};
    if (row.generation) {
        result["loadedName"] = row.loaded_name;
        result["loadedVersion"] = row.loaded_version;
        result["errorCode"] = row.error;
    }
    return result;
}
}

// 对照目录与实际实例，重扫同一份清单保留修订，原代次退出或被替换会产生新修订。
// 步骤：采样目录及代次 → 复用未变结果 → 合并磁盘与运行字段 → 发布有明确范围的版本。
std::shared_ptr<const PluginDirectory> sample_plugins(const plugins::PluginManager& manager, const std::shared_ptr<const PluginDirectory>& previous) {
    const auto catalog = manager.catalog();
    auto loaded = manager.loaded();
    const bool available = manager.directory_error().empty();
    const bool same_runtime = previous && previous->available==available && previous->loaded==loaded;
    if (same_runtime && previous->catalog==catalog) return previous;
    const bool same = same_runtime && previous->catalog->packages==catalog->packages &&
        previous->catalog->diagnostics.size()==catalog->diagnostics.size();
    auto next = std::make_shared<PluginDirectory>();
    next->revision = previous ? previous->revision : 0;
    if (!same) ++next->revision;
    next->available = available;
    next->catalog = catalog;
    next->loaded = std::move(loaded);

    for (const auto& [id,manifest] : catalog->packages) {
        auto& row = next->packages[id];
        row.name = manifest.name;
        row.version = manifest.version;
        row.installed = true;
    }
    for (const auto& live : next->loaded) {
        const auto instance = manager.instance(live.id);
        auto& row = next->packages[live.id];
        row.loaded_name = instance->plugin_name();
        row.loaded_version = instance->plugin_version();
        if (!row.installed) { row.name = row.loaded_name; row.version = row.loaded_version; }
        row.generation = live.generation;
        row.error = live.error;
        row.state = package_state(live);
    }
    return next;
}

// 从请求借用有界、非空的不透明身份。
const std::string& plugin_identity(const Json& value, std::size_t limit) {
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size()>limit) throw std::invalid_argument("plugins: invalid identity");
    return text;
}

// 解析完整规范的十进制字符串，超出 64 位或混入其他字符时拒绝。
std::uint64_t plugin_number(const Json& value) {
    const auto& text = plugin_identity(value,20);
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(text.data(),text.data()+text.size(),number);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || (text.size()>1 && text.front()=='0'))
        throw std::invalid_argument("plugins: invalid numeric identity");
    return number;
}

// 按当前后台和完整十进制修订读取不可变包目录。
// 步骤：核对读口字段 → 区分后台或修订失效与读取失败 → 编码同次采样的包行。
nlohmann::json plugin_directory_request(const Host& host, const Json& request) {
    const bool bound = request.contains("instance"), pinned = request.contains("revision");
    if (request.at("type")!="plugins.list" || (pinned && !bound) || request.size()!=3u+bound+pinned)
        throw std::invalid_argument("plugins: invalid directory request");
    const auto view = host.packages();
    if (!view) throw std::runtime_error("plugins: directory unavailable");
    const bool wrong_instance = bound && plugin_identity(request.at("instance"),64)!=host.instance();
    const bool wrong_revision = pinned && plugin_number(request.at("revision"))!=view->revision;
    const bool stale = wrong_instance || wrong_revision;
    if (!stale && !view->available) throw std::runtime_error("plugins: catalog scan unavailable");
    auto rows = Json::array();
    if (!stale) for (const auto& [id,row] : view->packages) rows.push_back(package_json(id,row));
    return {{"version","1"},{"event","plugins.directory"},{"request",request.at("request")},{"instance",host.instance()},
        {"revision",std::to_string(view->revision)},{"state",stale ? "stale" : "ready"},{"packages",std::move(rows)},
        {"unavailableCount",stale ? 0u : view->catalog->diagnostics.size()}};
}
}
