#include <dafeiyu/plugin_companion.hpp>
#include <dafeiyu/companion_abi_v2.h>
#include <algorithm>

namespace dfy::plugins {
namespace {
// 将插件局部名称复制为宿主字符串，拒绝跨包路径。
std::string local_name(dfy_v2_string_view value) {
    if (!value.data || !value.length || value.length > 32) throw std::invalid_argument("companion: invalid local name length");
    std::string name(value.data, static_cast<std::size_t>(value.length));
    if (!std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    })) throw std::invalid_argument("companion: invalid local name");
    return name;
}
}

// 读取可选角色声明，以同代只读工具检查状态查询。
// 步骤：查询可选能力 → 在调用内复制借用字段 → 验证原实例的只读登记。
std::optional<CompanionOffer> companion_offer(PluginInstance& instance, const agent::ToolCatalog& catalog) {
    auto capability = instance.query<dfy_v2_companion_api>(DFY_V2_COMPANION_INTERFACE, DFY_V2_COMPANION_VERSION);
    if (!capability) return {};
    auto result = instance.invoke(*capability, [&](const auto& api, auto* native) {
        dfy_v2_companion_description value{};
        value.struct_size = sizeof(value);
        dfy_v2_error error{};
        if (!api.describe || api.describe(native, &value, &error) != DFY_V2_OK || error.length ||
            value.struct_size != sizeof(value) || value.reserved) throw std::runtime_error("companion: invalid descriptor");
        return CompanionOffer{instance.generation(), std::string(instance.plugin_id()) + "/" + local_name(value.state_tool),
            local_name(value.state_topic)};
    });
    const auto scope = catalog.snapshot(std::span(&result.state_tool, 1), false);
    const auto descriptions = scope.descriptions();
    if (descriptions.size() != 1 || descriptions.front().instance != result.generation ||
        descriptions.front().changes_state) throw std::invalid_argument("companion: unbound state tool");
    return result;
}
}
