#include <dafeiyu/trace_identity.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::agent;
// 报告追踪关联或版本信息错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证非法结构拒绝，不把损坏记录归入现有运行。
void rejects(const auto& action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid trace metadata accepted");
}
// 验证模型、工具和控制节点共享原尝试父级，跨运行同编号保持隔离。
void identities() {
    const TraceNodeId root{{1,2}}, step{{1,2},TraceNodeKind::step,3}, attempt{{1,2},TraceNodeKind::attempt,3,1};
    check(!trace_parent(root) && trace_parent(step) == root && trace_parent(attempt) == step,"trace parent chain broken");
    for (const auto kind : {TraceNodeKind::model,TraceNodeKind::tool,TraceNodeKind::control}) {
        const TraceNodeId node{{1,2},kind,3,1,8}; check(trace_parent(node) == attempt,"child assigned to wrong attempt");
        auto other = node; other.run.sequence = 9; check(trace_parent(other) != attempt,"foreign run merged into trace");
    }
    rejects([] { validate_trace_id({{1,2},TraceNodeKind::run,1}); });
    rejects([] { validate_trace_id({{1,2},TraceNodeKind::model,1,1,0}); });
    rejects([] { validate_trace_id({{0,2},TraceNodeKind::tool,1,1,1}); });
    rejects([] { validate_trace_id({{1,2},static_cast<TraceNodeKind>(99),1,1,1}); });
}
// 验证未知版本不被补值，模型配置只投影公开字段，工具清单保持有界。
// 步骤：读取空及公开模型标识 → 校验有效工具版本 → 拒绝重复、零版本和异常文本。
void versions() {
    const auto unknown = public_model_versions({});
    check(!unknown.provider && !unknown.model && !unknown.application_commit && !unknown.policy,"missing versions fabricated");
    dfy::model::Config config{"qwen","https://private.invalid/secret","fixture-model","PRIVATE_KEY_REFERENCE",123};
    auto value = public_model_versions(config);
    check(value.provider == "qwen" && value.model == "fixture-model" && !value.model_profile && value.tools.empty(),"public model projection changed");
    value.application_commit = "fixture-commit"; value.policy = 3;
    value.tools.push_back({"fixture/read",1,4,"1.0","schema-v1"}); validate_trace_versions(value);
    value.tools.push_back(value.tools.front()); rejects([&] { validate_trace_versions(value); }); value.tools.pop_back();
    value.tools[0].contract = 0; rejects([&] { validate_trace_versions(value); }); value.tools[0].contract.reset();
    value.build = "secret\nbody"; rejects([&] { validate_trace_versions(value); }); value.build.reset();
    value.tools.resize(65); rejects([&] { validate_trace_versions(value); });
}
}
// 验证纯元数据关联与版本，不访问服务、凭据或业务正文。
int main() {
    try { identities(); versions(); std::cout << "Trace identity checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
