#include <dafeiyu/tool_catalog.hpp>
#include <dafeiyu/tool_schema.hpp>
#include <dafeiyu/digest.hpp>
#include <dafeiyu/trace_identity.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <tuple>
#include <stdexcept>

namespace dfy::agent {
struct ToolEntry {
    const ToolDescription description;
    const ToolSchema schema;
    const runtime::DomainId registration;
    const ToolFactory factory;
    bool enabled = true, registered = true;
};
namespace {
// 固定参数保留 JSON 类型和完整对象内容，重复字段不能形成多义授权。
std::string exact_arguments(const std::string& text) {
    using Json = nlohmann::json;
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 12) throw std::invalid_argument("tools: fixed arguments too deep");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("tools: duplicate argument");
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    }).dump();
}
}
// 校验固定槽数量后准备目录，不接受可导致无界分配的容量。
ToolCatalog::ToolCatalog(std::size_t capacity) {
    if (!capacity || capacity > 64) throw std::invalid_argument("tools: capacity must be 1..64");
    entries_.resize(capacity);
}
// 目录退出只撤销自己的成员，快照不拥有延续授权的权力。
ToolCatalog::~ToolCatalog() { for (const auto& entry : entries_) if (entry) entry->registered = false; }
// 在发布注册前校验全部描述，失败不消耗槽位或改变已有实例。
// 步骤：校验身份与模型描述 → 拒绝名称冲突 → 准备参数契约 → 签发注册并发布。
runtime::DomainId ToolCatalog::add(ToolDescription description, ToolFactory factory) {
    if (description.id.empty() || description.id.size() > 128 || !description.instance || !description.version)
        throw std::invalid_argument("tools: identity, instance and version required");
    if (description.receipt_view_bytes && (description.receipt_view_bytes < 256 || description.receipt_view_bytes > 64*1024))
        throw std::invalid_argument("tools: receipt view must fit 256..65536 bytes");
    for (const auto c : description.id) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/'))
        throw std::invalid_argument("tools: invalid canonical ID");
    model::validate_request({{{model::Role::user,"catalog"}},{description.model}});
    for (const auto& entry : entries_) if (entry && (entry->description.id == description.id || entry->description.model.name == description.model.name))
        throw std::invalid_argument("tools: duplicate ID or model name");
    const auto slot = std::find(entries_.begin(),entries_.end(),nullptr);
    if (slot == entries_.end()) throw std::length_error("tools: catalog full");
    ToolSchema schema(description.model.parameters);
    description.model.parameters = nlohmann::json::parse(description.model.parameters).dump();
    description.schema_digest = "sha256:"+platform::sha256(description.model.parameters);
    TraceVersions metadata; metadata.tools.push_back({description.id,description.version,description.instance,description.plugin_version,description.schema_digest});
    validate_trace_versions(metadata);
    const auto registration = runtime::next_domain();
    *slot = std::make_shared<ToolEntry>(std::move(description),std::move(schema),registration,std::move(factory));
    return registration;
}
// 仅按当前注册身份查找，不从过期身份猜测目标。
std::shared_ptr<ToolEntry> ToolCatalog::find(runtime::DomainId registration) const {
    for (const auto& entry : entries_) if (entry && entry->registration == registration) return entry;
    throw std::invalid_argument("tools: unknown registration");
}
// 先撤销旧快照的执行资格，再释放当前目录槽。
void ToolCatalog::remove(runtime::DomainId registration) {
    const auto entry = find(registration); entry->registered = false;
    for (auto& slot : entries_) if (slot == entry) { slot.reset(); return; }
}
// 立即改变当前授权开关，旧快照在下次解析时观察此变更。
void ToolCatalog::enable(runtime::DomainId registration, bool enabled) { find(registration)->enabled = enabled; }
// 只允许宿主明确列出的工具进入模型可见集合，任一错误都不交付部分授权。
// 步骤：检查授予数量及重复 → 核对成员与变更权限 → 形成固定快照。
ToolSnapshot ToolCatalog::snapshot(std::span<const std::string> ids, bool allow_changes) const {
    if (ids.size() > 64) throw std::length_error("tools: too many grants");
    std::set<std::string> seen; std::vector<std::shared_ptr<ToolEntry>> selected;
    for (const auto& id : ids) {
        if (!seen.insert(id).second) throw std::invalid_argument("tools: duplicate grant");
        const auto entry = std::find_if(entries_.begin(),entries_.end(),[&](const auto& item) { return item && item->description.id == id; });
        if (entry == entries_.end() || !(*entry)->enabled || ((*entry)->description.changes_state && !allow_changes))
            throw std::invalid_argument("tools: grant unavailable or not authorized");
        selected.push_back(*entry);
    }
    return ToolSnapshot(std::move(selected));
}
// 投影固定成员中当前仍允许执行的描述，查询不扩大授权或改变注册生命周期。
std::vector<model::ToolDefinition> ToolSnapshot::definitions() const {
    std::vector<model::ToolDefinition> result;
    for (const auto& entry : entries_) if (entry->registered && entry->enabled && entry->factory) result.push_back(entry->description.model);
    return result;
}
// 由当前可用的原注册生成与授权排列无关的模型定义。
// 步骤：读取有效成员 → 按展示序与规范 ID 排序 → 返回有序模型契约。
std::vector<model::ToolDefinition> ToolSnapshot::ordered_definitions(std::vector<ToolDescription>* captured) const {
    auto descriptions = this->descriptions();
    std::vector<model::ToolDefinition> result;
    result.reserve(descriptions.size());
    if (captured) {
        for (const auto& item : descriptions) result.push_back(item.model);
        *captured = std::move(descriptions);
    } else for (auto& item : descriptions) result.push_back(std::move(item.model));
    return result;
}
// 按注册展示序和规范 ID 返回有效或原固定成员，供模型、规划和诊断复用。
std::vector<ToolDescription> ToolSnapshot::descriptions(bool available_only) const {
    std::vector<ToolDescription> result;
    for (const auto& entry : entries_) if (!available_only || (entry->registered && entry->enabled && entry->factory)) result.push_back(entry->description);
    std::ranges::sort(result, [](const ToolDescription& left, const ToolDescription& right) {
        return std::tie(left.display_order,left.id) < std::tie(right.display_order,right.id);
    });
    return result;
}
// 按固定成员描述判断授予范围，不因暂时禁用而允许观察入口绕过变更限制。
bool ToolSnapshot::permits_state_changes() const noexcept {
    for (const auto& entry : entries_) if (entry->description.changes_state) return true;
    return false;
}
// 子范围仅复制原授权成员，当前禁用或撤销状态仍由每次实际调用检查。
// 步骤：限制成员数量 → 拒绝重复及范围外 ID → 保留原注册身份形成子快照。
ToolSnapshot ToolSnapshot::restrict_to(std::span<const std::string> ids) const {
    if (ids.size() > 64) throw std::length_error("tools: too many subset grants");
    std::set<std::string> seen; std::vector<std::shared_ptr<ToolEntry>> selected; selected.reserve(ids.size());
    for (const auto& id : ids) {
        if (!seen.insert(id).second) throw std::invalid_argument("tools: duplicate subset grant");
        const auto entry = std::find_if(entries_.begin(),entries_.end(),[&](const auto& item) { return item->description.id == id; });
        if (entry == entries_.end()) throw std::invalid_argument("tools: subset outside original grant");
        selected.push_back(*entry);
    }
    ToolSnapshot result(std::move(selected));
    result.fixed_arguments_ = fixed_arguments_;
    result.context_ = context_;
    return result;
}

// 将宿主单独签发的只读能力加入本轮范围，原注册和撤权仍由各自目录持有。
// 步骤：拒绝扩大固定参数委托 → 检查只读属性及命名冲突 → 保留原上下文绑定和新能力。
ToolSnapshot ToolSnapshot::include_reader(const ToolSnapshot& reader) const {
    if (fixed_arguments_ || reader.fixed_arguments_ || reader.context_ || reader.entries_.size()!=1 || entries_.size()>=64)
        throw std::invalid_argument("tools: invalid reader grant");
    const auto& extra = reader.entries_.front();
    if (extra->description.changes_state || !extra->registered || !extra->enabled || !extra->factory)
        throw std::invalid_argument("tools: unavailable read-only capability");
    for (const auto& existing : entries_)
        if (existing->description.id==extra->description.id || existing->description.model.name==extra->description.model.name)
            throw std::invalid_argument("tools: duplicate reader grant");
    auto result = *this;
    result.entries_.push_back(extra);
    return result;
}

// 固定由宿主签发的来源，子范围不得替换该来源。
ToolSnapshot ToolSnapshot::bind_context(const std::string& context) const {
    if (context.size() > 4096) throw std::invalid_argument("tools: oversized context");
    const auto canonical = exact_arguments(context);
    if (canonical.front() != '{' || (context_ && *context_ != canonical))
        throw std::invalid_argument("tools: invalid context binding");
    auto result = *this;
    result.context_ = std::make_shared<const std::string>(canonical);
    return result;
}

// 按宿主明确的委托把一个原注册及其参数完整固定。
// 步骤：收紧到原成员 → 验证有效契约 → 保留规范参数，拒绝替换已有约束。
ToolSnapshot ToolSnapshot::bind_arguments(const std::string& id, const std::string& arguments) const {
    auto result = restrict_to(std::span(&id, 1));
    const auto& entry = result.entries_.front();
    if (!entry->registered || !entry->enabled || !entry->factory) throw std::invalid_argument("tools: unavailable binding");
    entry->schema.validate(arguments);
    const auto canonical = exact_arguments(arguments);
    if (fixed_arguments_ && *fixed_arguments_ != canonical) throw std::invalid_argument("tools: cannot replace fixed arguments");
    result.fixed_arguments_ = std::make_shared<const std::string>(canonical);
    return result;
}

// 仅从本运行的成员中选择目标，并在使用前重新核对授权及参数。
// 步骤：定位快照内名称 → 检查撤销和禁用 → 校验参数 → 返回原实例和注册身份。
ToolSelection ToolSnapshot::resolve(const std::string& name, const std::string& arguments) const {
    for (const auto& entry : entries_) if (entry->description.model.name == name) {
        if (!entry->registered) throw ToolRejection(R"({"stage":"scope","reason":"revoked"})");
        if (!entry->enabled) throw ToolRejection(R"({"stage":"scope","reason":"disabled"})");
        entry->schema.validate(arguments);
        if (fixed_arguments_ && exact_arguments(arguments) != *fixed_arguments_)
            throw ToolRejection(R"({"stage":"scope","reason":"outside_fixed_arguments"})");
        return {entry->description,entry->registration};
    }
    throw ToolRejection(R"({"stage":"scope","reason":"not_granted"})");
}
// 先验证选择和参数，再准备不立即执行的驱动，授权撤销仍能阻止首次实际调用。
// 步骤：解析原实例 → 核对实现 → 捕获原注册并设置调用前校验 → 签发工作身份。
std::unique_ptr<ToolDriver> ToolSnapshot::prepare(AttemptId attempt, model::ToolCall call, ToolResources resources) const {
    const auto selection = resolve(call.name,call.arguments);
    for (const auto& entry : entries_) if (entry->registration == selection.registration) {
        if (!entry->factory) throw std::invalid_argument("tools: implementation unavailable");
        ToolFactory guarded = [entry, context = context_](const ToolInvocation& input) {
            if (!entry->registered)
                return completed_tool({false,ToolEffect::not_applied,R"({"stage":"before_start","reason":"revoked"})",ToolError::unavailable});
            if (!entry->enabled)
                return completed_tool({false,ToolEffect::not_applied,R"({"stage":"before_start","reason":"disabled"})",ToolError::unavailable});
            if (!context) return entry->factory(input);
            auto native = input;
            native.call.arguments = "{\"context\":" + *context + ",\"arguments\":" + input.call.arguments + "}";
            return entry->factory(native);
        };
        return std::make_unique<ToolDriver>(ToolInvocation{attempt,runtime::next_domain(),std::move(call)},std::move(guarded),std::move(resources));
    }
    throw std::logic_error("tools: missing selected registration");
}
}
