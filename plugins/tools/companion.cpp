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

// 读取插件声明的伙伴技能集合，不把消息收件和管理命令自动暴露给模型。
// 步骤：查询可选表 → 逐项复制有界局部名 → 核对同实例注册及唯一性。
std::vector<std::string> companion_skills(PluginInstance& instance, const agent::ToolCatalog& catalog) {
    auto capability = instance.query<dfy_v2_companion_skills_api>(DFY_V2_COMPANION_SKILLS_INTERFACE, DFY_V2_COMPANION_SKILLS_VERSION);
    if (!capability) return {};
    auto ids = instance.invoke(*capability, [&](const auto& api, auto* native) {
        std::vector<std::string> result;
        if (!api.at) throw std::runtime_error("companion: incomplete skills table");
        for (uint32_t index = 0; index <= 64; ++index) {
            dfy_v2_error error{};
            dfy_v2_string_view value{};
            const auto code = api.at(native, index, &value, &error);
            if (code == DFY_V2_NOT_FOUND && !error.length) return result;
            if (index == 64 || code != DFY_V2_OK || error.length) throw std::runtime_error("companion: invalid skills table");
            result.push_back(std::string(instance.plugin_id()) + "/" + local_name(value));
        }
        throw std::logic_error("companion: unterminated skills table");
    });
    const auto entries = catalog.snapshot(ids, true).descriptions();
    if (entries.size() != ids.size() || std::ranges::any_of(entries, [&](const auto& entry) { return entry.instance != instance.generation(); }))
        throw std::invalid_argument("companion: invalid skill registration");
    return ids;
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

// 仅承认活动插件自己注册的同代工具，已配对实例可以提供额外的玩家入口。
// 步骤：查询可选用途表 → 复制并限定局部 ID → 核对真实注册和副作用类别。
std::optional<std::string> companion_route(PluginInstance& instance, const agent::ToolCatalog& catalog,
    std::string_view purpose, bool changes_state) {
    auto capability = instance.query<dfy_v2_companion_routes_api>(DFY_V2_COMPANION_ROUTES_INTERFACE, DFY_V2_COMPANION_ROUTES_VERSION);
    if (!capability) return {};
    auto local = instance.invoke(*capability, [&](const auto& api, auto* native) -> std::optional<std::string> {
        if (!api.resolve) throw std::runtime_error("companion: invalid routes table");
        dfy_v2_string_view output{};
        dfy_v2_error error{};
        const auto code = api.resolve(native, {purpose.data(), purpose.size()}, &output, &error);
        if (code == DFY_V2_NOT_FOUND && !error.length && !output.data && !output.length) return {};
        if (code != DFY_V2_OK || error.length) throw std::runtime_error("companion: route unavailable");
        return local_name(output);
    });
    if (!local) return {};
    std::string id = std::string(instance.plugin_id()) + "/" + *local;
    const auto entries = catalog.snapshot(std::span(&id, 1), changes_state).descriptions();
    if (entries.size() != 1 || entries.front().instance != instance.generation() || entries.front().changes_state != changes_state)
        throw std::invalid_argument("companion: wrong route binding");
    return id;
}
}
