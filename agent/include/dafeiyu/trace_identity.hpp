#pragma once
#include <dafeiyu/model_config.hpp>
#include <dafeiyu/run_identity.hpp>
#include <optional>
#include <vector>
#include <chrono>

namespace dfy::agent {
enum class TraceNodeKind { run, step, attempt, model, tool, control };
// 命名空间由诊断宿主提供；节点身份仍关联原运行，不作为当前执行或审批凭证。
struct TraceNodeId {
    RunId run;
    TraceNodeKind kind = TraceNodeKind::run;
    std::uint64_t step = 0, attempt = 0, local = 0;
    // 按完整运行和节点路径比较，不把不同种类或步骤的局部编号混用。
    bool operator==(const TraceNodeId&) const = default;
};
struct ToolTraceVersion {
    std::string id;
    std::optional<std::uint32_t> contract;
    std::optional<runtime::DomainId> instance;
    std::optional<std::string> plugin, schema;
};
// 只接收已公开的版本标识；缺失保持未知，不接收配置对象、业务正文或密钥。
struct TraceVersions {
    std::optional<std::string> application_commit, build, prompt_template, provider, model, model_profile;
    std::optional<std::uint64_t> policy;
    std::vector<ToolTraceVersion> tools;
    std::optional<std::string> public_model_config, execution_policy, control_extension;
};
// 宿主显式提供的公开版本标签；供应商、模型、策略和工具版本由实际运行另行采集。
struct RunVersionLabels {
    std::optional<std::string> application_commit, build, prompt_template, model_profile;
    std::optional<std::string> control_extension;
};
struct VersionDifference {
    std::vector<std::string> changed, unknown;
};
struct RunOptions;
// 校验节点层级，拒绝不完整路径及未知种类。
void validate_trace_id(const TraceNodeId& id);
// 无分配地判断节点层级，供执行期间的故障隔离投影使用。
bool valid_trace_id(const TraceNodeId& id) noexcept;
// 由合法身份计算唯一父节点；根运行没有父节点。
std::optional<TraceNodeId> trace_parent(const TraceNodeId& id);
// 校验有界公开版本清单，空可选值保持未知，不自动补版本。
void validate_trace_versions(const TraceVersions& versions);
// 仅提取公开供应商和模型名，不读取凭据变量、端点或配置正文。
TraceVersions public_model_versions(const model::Config& config);
// 固定允许公开的配置摘要及实际工具版本，不读取端点、凭据引用或扩展捕获内容。
TraceVersions public_run_versions(const model::Config& config, const RunOptions& options, std::chrono::milliseconds effective_timeout);
// 比较公开软件／配置版本；缺失单列为未知，进程内实例代次不冒充软件变化，不执行场景。
VersionDifference compare_public_versions(const TraceVersions& previous, const TraceVersions& current);
}
