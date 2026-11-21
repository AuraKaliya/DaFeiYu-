#include <dafeiyu/trace_identity.hpp>
#include <dafeiyu/utf8.hpp>
#include <dafeiyu/run_options.hpp>
#include <dafeiyu/digest.hpp>
#include <nlohmann/json.hpp>
#include <set>
#include <algorithm>
#include <stdexcept>

namespace dfy::agent {
namespace {
// 校验公开标识文本的长度、编码和控制字符，避免版本记录包含任意多行正文。
void version_text(const std::string& value) {
    if (value.empty() || value.size() > 128 || !runtime::valid_utf8(value) || value.find_first_not_of(' ') == std::string::npos)
        throw std::invalid_argument("trace: invalid public version identifier");
    for (const unsigned char byte : value) if (byte < 32 || byte == 127) throw std::invalid_argument("trace: control character in version");
}
// 只校验显式提供的标识，缺失版本不构造伪默认值。
void optional_text(const std::optional<std::string>& value) { if (value) version_text(*value); }
}
// 直接判断运行及分层路径，错误身份不构造异常或分配错误文本。
// 步骤：检查运行身份 → 按节点类型核对路径字段 → 返回合法性。
bool valid_trace_id(const TraceNodeId& id) noexcept {
    if (!id.run.session || !id.run.sequence) return false;
    switch (id.kind) {
    case TraceNodeKind::run:
        if (!id.step && !id.attempt && !id.local) return true;
        break;
    case TraceNodeKind::step:
        if (id.step && !id.attempt && !id.local) return true;
        break;
    case TraceNodeKind::attempt:
        if (id.step && id.attempt && !id.local) return true;
        break;
    case TraceNodeKind::model:
    case TraceNodeKind::tool:
    case TraceNodeKind::control:
        if (id.step && id.attempt && id.local) return true;
        break;
    }
    return false;
}
// 在构造和查询边界以明确错误报告不合法身份，执行期间使用无分配判断。
void validate_trace_id(const TraceNodeId& id) {
    if (!valid_trace_id(id)) throw std::invalid_argument("trace: invalid node hierarchy");
}
// 只从原路径派生父节点，局部编号不跨运行关联。
std::optional<TraceNodeId> trace_parent(const TraceNodeId& id) {
    validate_trace_id(id);
    if (id.kind == TraceNodeKind::run) return std::nullopt;
    if (id.kind == TraceNodeKind::step) return TraceNodeId{id.run,TraceNodeKind::run};
    if (id.kind == TraceNodeKind::attempt) return TraceNodeId{id.run,TraceNodeKind::step,id.step};
    return TraceNodeId{id.run,TraceNodeKind::attempt,id.step,id.attempt};
}
// 检查完整公开版本清单，拒绝重复工具及显式零版本。
// 步骤：校验应用和模型标识 → 检查策略及工具容量 → 核对每项工具版本和实例。
void validate_trace_versions(const TraceVersions& versions) {
    optional_text(versions.application_commit); optional_text(versions.build); optional_text(versions.prompt_template);
    optional_text(versions.provider); optional_text(versions.model); optional_text(versions.model_profile);
    optional_text(versions.public_model_config); optional_text(versions.execution_policy); optional_text(versions.control_extension);
    optional_text(versions.persona_profile);
    if ((versions.policy && !*versions.policy) || versions.tools.size() > 64) throw std::invalid_argument("trace: invalid version limits");
    std::set<std::string> tools;
    for (const auto& tool : versions.tools) {
        version_text(tool.id); optional_text(tool.plugin); optional_text(tool.schema);
        if (!tools.insert(tool.id).second || (tool.contract && !*tool.contract) || (tool.instance && !*tool.instance))
            throw std::invalid_argument("trace: invalid or duplicate tool version");
    }
}
// 提取确定可公开的模型标识，空测试配置继续保留未知状态。
TraceVersions public_model_versions(const model::Config& config) {
    TraceVersions result;
    if (!config.provider.empty()) result.provider = config.provider;
    if (!config.model.empty()) result.model = config.model;
    validate_trace_versions(result); return result;
}
// 只对显式允许的字段取摘要，缺失代码／模板标识不伪造为当前版本。
// 步骤：复制公开标识 → 摘要公开模型及基础执行配置 → 关联实际工具版本 → 校验清单。
TraceVersions public_run_versions(const model::Config& config, const RunOptions& options, std::chrono::milliseconds effective_timeout) {
    using Json = nlohmann::json;
    auto result = public_model_versions(config); const auto& labels = options.version_labels;
    result.application_commit = labels.application_commit; result.build = labels.build; result.prompt_template = labels.prompt_template;
    result.model_profile = labels.model_profile; result.policy = options.policy.revision; result.control_extension = labels.control_extension;
    result.persona_profile = labels.persona_profile;
    if (!options.extension && !options.text_hook && !result.control_extension) result.control_extension = "none";
    if (result.provider && result.model && config.timeout_ms > 0) result.public_model_config = "sha256:"+platform::sha256(Json{
        {"provider",config.provider},{"model",config.model},{"timeout_ms",config.timeout_ms}}.dump());
    Json policy{{"revision",options.policy.revision},{"model_limit",options.policy.model_limit},{"tool_limit",options.policy.tool_limit},
        {"allow_observation",options.policy.allow_observation},{"confirm_changes",options.policy.confirm_changes},{"timeout_ms",effective_timeout.count()},
        {"goal_update_limit",options.goal_update_limit},{"observation_rebind_limit",options.observation_rebind_limit},
        {"confirmation_limit",options.confirmation_limit},{"no_progress_limit",options.no_progress_limit},{"planning",nullptr},{"parent_budget",nullptr}};
    if (options.planning) policy["planning"] = {{"steps",options.planning->maximum_steps},{"revisions",options.planning->maximum_revisions},{"text_bytes",options.planning->maximum_text_bytes}};
    if (options.observation_action) policy["observation_action_revision"] = options.observation_action->revision();
    if (options.budget) { const auto limit = options.budget->limit(); policy["parent_budget"] = {{"items",limit.items},{"bytes",limit.bytes}}; }
    result.execution_policy = "sha256:"+platform::sha256(policy.dump());
    if (options.tools) for (const auto& tool : options.tools->descriptions(false)) result.tools.push_back({tool.id,tool.version,tool.instance,tool.plugin_version,tool.schema_digest});
    validate_trace_versions(result); return result;
}
// 只比较有依据的公开版本，未知不能被当成相等，运行期身份不跨进程比较。
// 步骤：校验清单 → 比较固定标签及摘要 → 按规范工具 ID 匹配 → 报告已知差异和缺失信息。
VersionDifference compare_public_versions(const TraceVersions& previous, const TraceVersions& current) {
    validate_trace_versions(previous); validate_trace_versions(current); VersionDifference result;
    const auto compare = [&](std::string name,const auto& left,const auto& right) {
        if (!left || !right) result.unknown.push_back(std::move(name));
        else if (*left != *right) result.changed.push_back(std::move(name));
    };
    compare("application_commit",previous.application_commit,current.application_commit); compare("build",previous.build,current.build);
    compare("prompt_template",previous.prompt_template,current.prompt_template); compare("provider",previous.provider,current.provider);
    compare("persona_profile",previous.persona_profile,current.persona_profile);
    compare("model",previous.model,current.model); compare("model_profile",previous.model_profile,current.model_profile);
    compare("policy_revision",previous.policy,current.policy); compare("public_model_config",previous.public_model_config,current.public_model_config);
    compare("execution_policy",previous.execution_policy,current.execution_policy); compare("control_extension",previous.control_extension,current.control_extension);
    std::set<std::string> ids;
    for (const auto& tool : previous.tools) ids.insert(tool.id);
    for (const auto& tool : current.tools) ids.insert(tool.id);
    for (const auto& id : ids) {
        const auto left = std::find_if(previous.tools.begin(),previous.tools.end(),[&](const auto& item) { return item.id == id; });
        const auto right = std::find_if(current.tools.begin(),current.tools.end(),[&](const auto& item) { return item.id == id; });
        const auto prefix = "tool/"+id+"/";
        if (left == previous.tools.end()) result.changed.push_back(prefix+"added");
        else if (right == current.tools.end()) result.changed.push_back(prefix+"removed");
        else {
            compare(prefix+"contract",left->contract,right->contract); compare(prefix+"plugin",left->plugin,right->plugin);
            compare(prefix+"schema",left->schema,right->schema);
        }
    }
    return result;
}
}
