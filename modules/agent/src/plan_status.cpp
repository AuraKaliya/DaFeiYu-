#include <dafeiyu/session.hpp>
#include "diagnostic_fields.hpp"
#include <nlohmann/json.hpp>

namespace dfy::agent {
namespace {
using Json = nlohmann::json;
// 项目正文超出单项展示预算时保留 UTF-8 片段，原计划仍由步骤 ID 和修订号定位。
std::string todo_text(const std::string& text,bool& shortened) {
    if (Json(text).dump().size()<=224) return text;
    std::size_t end = std::min<std::size_t>(40,text.size());
    while (end<text.size() && (static_cast<unsigned char>(text[end])&0xc0)==0x80) --end;
    shortened = true;
    return text.substr(0,end)+"…";
}
// 声明宿主待办的正文、原计划修订、验证、等待及时间字段。
std::string todo_definitions() {
    Json properties = Json::object();
    for (auto key : {"id","objective","criterion","state","waiting","evidence"}) properties[key]={{"type","string"}};
    for (auto key : {"goalRevision","planRevision","originPlanRevision","executionStep","executionAttempt","createdUtcMs","updatedUtcMs"})
        properties[key]={{"type","integer"}};
    properties["textAbbreviated"]={{"type","boolean"}};
    Json schema{{"type","object"},{"properties",std::move(properties)},{"additionalProperties",false},
        {"required",{"id","objective","criterion","state","waiting","goalRevision","planRevision","originPlanRevision"}}};
    return Json::array({{{"key","host.plan_todo"},{"version",1},{"description","宿主已安装的计划待办，原工作退出和宿主验证分别记录。时间来自计划的接纳和最近一次写入。"},
        {"defaultAttention",{{"mode","implicit"}}},{"valueSchema",std::move(schema)}}}).dump();
}
}
// 从当前计划的原定义、工作句柄与宿主验证采样，只让下一项尚未结束的步骤持有任务期关注。
// 步骤：核对已安装修订 → 按原 ID 冻结各步骤 → 绑定原工作及证据 → 选择当前相关待办。
std::optional<DecisionStatusSource> TurnRequest::plan_status(std::uint64_t step,std::int64_t utc) const {
    if (!plan_) return {};
    const auto plan = plan_->snapshot();
    if (!plan.definition) return {};
    static const auto definitions = todo_definitions();
    const auto binding = plan.definition->binding();
    const auto task = "plan/"+std::to_string(binding.run.session)+"/"+std::to_string(binding.run.sequence);
    const auto prefix = task+"/"+std::to_string(binding.goal_revision)+"/";
    Json samples = Json::array(), watches = Json::array();
    for (std::size_t i=0;i<plan.steps.size();++i) {
        const auto& item = plan.steps[i];
        const auto& original = plan.definition->steps()[i];
        bool short_text = false;
        Json value{{"id",original.id},{"objective",todo_text(original.objective,short_text)},{"criterion",todo_text(original.completion_criterion,short_text)},
            {"goalRevision",binding.goal_revision},{"planRevision",binding.plan_revision},{"originPlanRevision",item.key.plan.plan_revision},
            {"state",diagnostic_detail::EnumNames<PlanStepState>::values[static_cast<unsigned>(item.state)]},
            {"waiting",diagnostic_detail::EnumNames<PlanWait>::values[static_cast<unsigned>(item.waiting)]}};
        if (short_text) value["textAbbreviated"] = true;
        if (item.created_utc_ms) value["createdUtcMs"] = *item.created_utc_ms;
        if (item.updated_utc_ms) value["updatedUtcMs"] = *item.updated_utc_ms;
        if (item.execution) { value["executionStep"] = item.execution->step.sequence; value["executionAttempt"] = item.execution->sequence; }
        if (!item.evidence.empty()) value["evidence"] = item.evidence;
        const auto subject = prefix+original.id;
        samples.push_back({{"key","host.plan_todo"},{"version",1},{"subject",subject},{"fact",step},{"sampledUtcMs",utc},{"value",std::move(value)}});
        if (!plan.stopped && watches.empty() && item.state!=PlanStepState::completed && item.state!=PlanStepState::skipped)
            watches.push_back({{"key","host.plan_todo"},{"version",1},{"subject",subject},{"mode","continuous"},{"task",task}});
    }
    return DecisionStatusSource{"host",definitions,samples.dump(),watches.dump()};
}
}
