#include <dafeiyu/plan_definition.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
// 报告计划候选契约与固定范围不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证非法、超量或越权计划明确拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid plan accepted");
}
// 准备不含执行代码的顺序步骤，仅声明目标和待验证的完成条件。
std::vector<PlanStepDefinition> steps() {
    return {{"read","读取当前状态","取得受控读取回执",{"fixture/read"}},{"summarize","汇总已确认事实","形成文本回答",{}}};
}
// 验证完整范围及目标版本固定，候选构造不执行工具，旧值不随后续候选改变。
// 步骤：固定原工具范围 → 发布两个独立候选 → 拒绝越权和重复定义 → 核对原计划及零执行。
void scope() {
    ToolCatalog catalog; unsigned calls = 0;
    catalog.add({"fixture/read",1,1,{"read","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},false,true},
        [&](const auto&) { ++calls; return completed_tool({true,ToolEffect::not_applied,"",ToolError::none}); });
    const std::vector<std::string> ids{"fixture/read"}; PlanScope scope({1,2},3,catalog.snapshot(ids));
    const auto plan = PlanDefinition::create(scope,1,steps()); const auto next = PlanDefinition::create(scope,2,{{"answer","整理资料","宿主确认结果",{}}});
    check(plan.binding() == PlanBinding{{1,2},3,1} && plan.steps().size() == 2 && next.binding().plan_revision == 2 && !calls &&
        scope.permits("fixture/read") && !scope.permits("fixture/write"),"candidate changed scope or executed work");
    auto invalid = steps(); invalid[0].tools = {"fixture/write"}; rejects([&] { PlanDefinition::create(scope,1,invalid); });
    invalid = steps(); invalid[1].id = invalid[0].id; rejects([&] { PlanDefinition::create(scope,1,invalid); });
    invalid = steps(); invalid[0].tools.push_back("fixture/read"); rejects([&] { PlanDefinition::create(scope,1,invalid); });
    check(plan.steps()[0].tools[0] == "fixture/read" && !calls,"rejected candidate changed original plan");
}
// 验证无工具文本计划和各类有界输入，不把空白或坏编码作为有效目标。
// 步骤：接纳有界文本候选 → 拒绝版本和数量错误 → 拒绝正文、编码及总量错误。
void limits() {
    const PlanScope scope({1,1},1,{}, {1,2,32}); const std::vector<PlanStepDefinition> plain{{"text","回答","输出文本",{}}};
    check(PlanDefinition::create(scope,1,plain).text_bytes() > 0,"text-only plan refused");
    rejects([&] { PlanDefinition::create(scope,0,plain); }); rejects([&] { PlanDefinition::create(scope,3,plain); });
    rejects([&] { PlanDefinition::create(scope,1,{}); }); rejects([&] { PlanDefinition::create(scope,1,{plain[0],plain[0]}); });
    auto bad = plain; bad[0].objective = " \n"; rejects([&] { PlanDefinition::create(scope,1,bad); });
    bad = plain; bad[0].completion_criterion = std::string(1,static_cast<char>(0xff)); rejects([&] { PlanDefinition::create(scope,1,bad); });
    bad = plain; bad[0].objective = std::string(32,'x'); rejects([&] { PlanDefinition::create(scope,1,bad); });
    bad = plain; bad[0].id = "../step"; rejects([&] { PlanDefinition::create(scope,1,bad); });
}
}
// 只验证预设计划候选，不调用模型或解释自然语言完成条件。
int main() {
    try { scope(); limits(); std::cout << "Plan definition checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
