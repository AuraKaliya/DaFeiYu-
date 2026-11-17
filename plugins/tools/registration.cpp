#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/tool_schema.hpp>
#include <dafeiyu/tool_abi_v2.h>
#include <dafeiyu/utf8.hpp>

namespace dfy::plugins {
namespace {
// 在能力调用保护内复制有界 UTF-8 字符串，借用数据不进入目录。
std::string copy(dfy_v2_string_view value, std::size_t limit) {
    if (value.length > limit || (value.length && !value.data)) throw std::invalid_argument("plugin tools: invalid string view");
    std::string result; if (value.length) result.assign(value.data,static_cast<std::size_t>(value.length));
    if (!runtime::valid_utf8(result)) throw std::invalid_argument("plugin tools: invalid UTF-8");
    return result;
}
// 将包命名空间映射为模型可接受的稳定名称，名称冲突仍由目录明确拒绝。
// 步骤：摘要全限定 ID → 编码固定十六进制 → 附加可读局部名称。
std::string model_name(const std::string& id, const std::string& local) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char byte : id) { hash ^= byte; hash *= UINT64_C(1099511628211); }
    std::string result = "p_";
    for (int shift = 60; shift >= 0; shift -= 4) result.push_back("0123456789abcdef"[(hash >> shift) & 15]);
    return result+"_"+local;
}
struct Prepared { agent::ToolDescription description; agent::ToolFactory factory; };
}
// 将可选 C 工具描述完整复制到宿主后注册，任一发布失败回滚本实例已注册成员。
// 步骤：查询精确能力并核对函数 → 读取全部有界描述 → 校验契约并准备工厂 → 发布或回滚注册。
PluginTools::PluginTools(const std::shared_ptr<PluginInstance>& instance, std::shared_ptr<agent::ToolCatalog> catalog)
    : catalog_(std::move(catalog)) {
    if (!instance || !catalog_) throw std::invalid_argument("plugin tools: instance and catalog required");
    auto capability = instance->query<dfy_v2_tool_api>(DFY_V2_TOOL_INTERFACE,DFY_V2_TOOL_VERSION);
    if (!capability) return;
    const auto count = instance->invoke(*capability,
    // 核对完整工具表并读取有界数量，任何非法回执都在注册前拒绝。
    // 步骤：检查必需函数 → 调用只读数量接口 → 校验状态及上限。
    [](const auto& api,auto* native) {
        if (!api.count || !api.describe || !api.begin || !api.poll || !api.request_cancel || !api.release)
            throw std::runtime_error("plugin tools: incomplete interface");
        std::uint32_t count = 0; dfy_v2_error error{};
        if (api.count(native,&count,&error) != DFY_V2_OK || error.length || count > 64)
            throw std::runtime_error("plugin tools: invalid count");
        return count;
    });
    std::vector<Prepared> prepared; prepared.reserve(count); ids_.reserve(count); registrations_.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        auto entry = instance->invoke(*capability,
        // 借用数据只在当前能力保护内读取，返回已校验的宿主副本和弱实例工厂。
        // 步骤：读取布局 → 校验版本及标志 → 复制文本与 schema → 准备全限定身份和工厂。
        [&](const auto& api,auto* native) {
            dfy_v2_tool_description description{}; description.struct_size = sizeof(description); dfy_v2_error error{};
            if (api.describe(native,index,&description,&error) != DFY_V2_OK || error.length ||
                description.struct_size != sizeof(description) || !description.version || description.reserved ||
                (description.flags & ~(DFY_V2_TOOL_CHANGES_STATE | DFY_V2_TOOL_IDEMPOTENT)))
                throw std::runtime_error("plugin tools: invalid description");
            const auto local = copy(description.id,32);
            const auto id = std::string(instance->plugin_id())+"/"+local;
            agent::ToolDescription host{id,instance->generation(),description.version,
                {model_name(id,local),copy(description.description,1024),copy(description.parameters,64*1024)},
                (description.flags & DFY_V2_TOOL_CHANGES_STATE) != 0,(description.flags & DFY_V2_TOOL_IDEMPOTENT) != 0};
            const agent::ToolSchema schema(host.model.parameters);
            host.plugin_version = std::string(instance->plugin_version());
            return Prepared{std::move(host),native_tool_factory(instance,local,description.version)};
        });
        ids_.push_back(entry.description.id); prepared.push_back(std::move(entry));
    }
    try {
        for (auto& entry : prepared) registrations_.push_back(catalog_->add(std::move(entry.description),std::move(entry.factory)));
    } catch (...) { revoke(); throw; }
}
// 注册控制器退出只撤销自己的 ID，已开始工作仍由原驱动和实例负责。
PluginTools::~PluginTools() { revoke(); }
// 撤销本次确切注册，已经外部撤销的成员不影响其余清理。
void PluginTools::revoke() noexcept {
    for (const auto registration : registrations_) { try { catalog_->remove(registration); } catch (...) {} }
    registrations_.clear(); ids_.clear();
}
}
