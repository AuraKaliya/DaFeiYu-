#pragma once
#include <dafeiyu/plugin_manager.hpp>
#include <nlohmann/json_fwd.hpp>

namespace dfy::management {
struct PackageRow {
    std::string name, version, loaded_name, loaded_version;
    std::string state = "installed";
    runtime::DomainId generation = 0;
    std::uint32_t error = 0;
    bool installed = false;
    // 当前目录中已安装且尚无实例的包可提交加载。
    bool can_load() const noexcept { return installed && !generation; }
    // 已持有的实例可提交卸载或重试，已在停止的实例沿原工作等待。
    bool can_unload() const noexcept { return generation && state!="stopping"; }
};
struct PluginDirectory {
    std::uint64_t revision = 0;
    bool available = false;
    std::shared_ptr<const plugins::Catalog> catalog;
    std::vector<plugins::LoadedPlugin> loaded;
    std::map<std::string,PackageRow> packages;
};
class Host;
// 校验协议中的不透明身份，只借用请求本身的有界字符串。
const std::string& plugin_identity(const nlohmann::json& value, std::size_t limit);
// 在业务线程关联已发现包和实际运行代次，未变的快照供多个窗口共享。
std::shared_ptr<const PluginDirectory> sample_plugins(const plugins::PluginManager& manager, const std::shared_ptr<const PluginDirectory>& previous);
// 按后台及可选目录修订读取已发布的包状态。
nlohmann::json plugin_directory_request(const Host& host, const nlohmann::json& request);
}
