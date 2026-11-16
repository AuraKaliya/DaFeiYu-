#include <dafeiyu/trace_identity.hpp>
#include <dafeiyu/utf8.hpp>
#include <set>
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
}
