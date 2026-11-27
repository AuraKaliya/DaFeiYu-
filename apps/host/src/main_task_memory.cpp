#include "main_messages.hpp"
#include "task_reflection.hpp"
#include "run_log.hpp"
#include "trace_model.hpp"
#include <dafeiyu/digest.hpp>

namespace dfy::app {
using Json = nlohmann::json;

// 本体任务入口固定原运行、实际提供实例、用户要求和宿主事先约定的核验条件。
void MainMessages::watch_task(const agent::TurnDiagnostics& run, const std::string& goal, const std::string& tool,
    runtime::DomainId provider, const agent::ToolSnapshot& scope, std::optional<std::string> expected,std::optional<ComparisonTag> comparison) {
    reflection_->interrupt();
    Json task{{"origin",instance_+"/"+std::to_string(run.session)+"/"+std::to_string(run.turn)},{"goal",goal},{"tool",tool},
        {"provider",provider},{"changes",scope.permits_state_changes()},{"verification","unconfirmed"},{"works",Json::array()}};
    const auto offered = scope.definitions();
    if (offered.size()==1) task["wireName"] = offered.front().name;
    if (expected) task["expected"] = *expected;
    if (comparison) task["comparison"]={{"name",comparison->name},{"checkpoint",comparison->checkpoint}};
    watched_task_.emplace(agent::RunId{run.session,run.turn},std::move(task));
}

// 在原运行实际退出时一次取证，计划、派发、工作实效和独立读回分别保留。
// 步骤：核对注册的本体运行 → 收取实际工作 → 绑定宿主核验的变更和读回 → 保存待整理原件。
void MainMessages::task_exited(const agent::TurnRequest& run) {
    if (!watched_task_ || watched_task_->first!=run.run_id()) return;
    auto& task = watched_task_->second;
    task["utcMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto plan = run.plan();
    if (plan && plan->definition && !plan->definition->steps().empty()) task["plan"] = plan->definition->steps().front().objective;
    task["runResult"] = trace_format::states.at(static_cast<std::size_t>(run.operation()->state()));
    auto& works = task.at("works");
    run.visit_tool_evidence([&](const agent::ToolCapture& source) {
        auto fact = tool_capture_fact("main",source);
        if (task.at("tool")==fact.value("toolId",std::string{}) ||
            (!fact.contains("toolId") && task.contains("wireName") && fact.at("name")==task.at("wireName"))) task["primary"] = works.size();
        works.push_back(std::move(fact));
    });
    for (const auto& feedback : run.plan_feedback()) {
        if (!feedback.latest || works.size()!=3 || works[1].at("candidate")!=feedback.completed_tool ||
            !works[2].value("dispatched",false) || !works[2].value("succeeded",false)) continue;
        const auto match = feedback.latest->match;
        if (match==agent::ObservationMatch::unknown) continue;
        task["primary"] = 1;
        task["verification"] = match==agent::ObservationMatch::satisfied ? "confirmed" : "contradicted";
        task["feedback"] = feedback.request;
    }
    // 只在有独立前后读回时识别首次状态变化；单纯执行成功仍保留未核验状态。
    if (task.at("verification")=="contradicted") task["reason"] = "unexpected_result";
    else if (task.at("verification")=="confirmed" && works[0].at("output")!=works[2].at("output")) {
        task["reason"] = "first_transition";
        task["signature"] = platform::sha256(Json::array({task.at("tool"),works[0].at("toolId"),works[0].at("output"),works[2].at("output")}).dump());
    } else if (task.at("changes")==true && task.at("verification")=="unconfirmed") {
        for (std::size_t i=0;i<works.size();++i) {
            const auto& fact = works[i];
            if (task.at("tool")==fact.value("toolId",std::string{}) && fact.value("dispatched",false) &&
                fact.at("effect")!="not_applied") { task["reason"] = "unverified_effect"; task["primary"] = i; }
        }
    }
    if (unsaved_tasks_.empty()) {
        try { store_->save_task(task); idle_review_due_ = true; }
        catch (const std::runtime_error&) { storage_failed(); unsaved_tasks_.push_back(std::move(task)); }
    } else unsaved_tasks_.push_back(std::move(task));
    watched_task_.reset();
}

// 空闲时按优先级整理一项，持久化待办在重启后继续，写锁不会重新触发工具。
void MainMessages::review_idle_task(bool idle) {
    idle = idle && pending_.empty() && unsaved_tasks_.empty() && retry_.empty() && replies_.empty() &&
        std::chrono::steady_clock::now()>=retry_at_;
    if (idle) settle_tone();
    if (idle && idle_review_due_) {
        try { idle_review_due_ = store_->review_idle_task(); }
        catch (const std::runtime_error&) {
            storage_failed();
            retry_at_ = std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
        }
    }
    reflection_->advance(idle && !idle_review_due_);
}
// 本体关闭时回收独立模型调用的退出责任。
bool MainMessages::close_reflection() {
    return reflection_->close();
}

// 只把本轮查中的原件和来源送入私聊决策尾注。
agent::DecisionStatusSource MainMessages::understanding_source(std::uint64_t turn, const Json& selection) const {
    static const auto schema=[] {
        const Json str{{"type","string"}}, num{{"type","integer"}}, flag{{"type","boolean"}};
        const auto object=[](const Json& fields){return Json{{"type","object"},{"properties",fields},{"additionalProperties",false}};};
        Json facts=Json::object(), claim=Json::object(), item=Json::object();
        for (const auto key : {"taskRef","actionRef","receiptRef","baselineRef","readbackRef","before","expected","actual","effect","goal","verification","role"}) facts[key]=str;
        facts["challengedPrevious"]=flag; facts["cited"]=flag;
        for (const auto key : {"text","condition","when","predict","value"}) claim[key]=str;
        for (const auto key : {"reference","previousReference","revisedFrom","author","kind","relation","standing"}) item[key]=str;
        for (const auto key : {"supportCount","counterexampleCount","nextOffset"}) item[key]=num;
        item["claim"]=object(claim); item["facts"]={{"type","array"},{"items",object(facts)}};
        const auto value=object({{"limited",flag},{"ambiguous",flag},{"items",{{"type","array"},{"items",object(item)}}}});
        return Json::array({{{"key","memory.understanding"},{"version",2},{"description","本体个人解释和原任务事实，反例优先"},
            {"defaultAttention",{{"mode","explicit"}}},{"snapshotRead",false},{"valueSchema",value}}}).dump();
    }();
    return {"memory",schema,Json::array({{{"key","memory.understanding"},{"version",2},{"subject",store_->conversation()},
        {"fact",turn},{"value",selection}}}).dump()};
}
// 在送模前按完整任务号复核档案归属和有效性，主题及回执号均不触发接续。
// 步骤：检查任务号 → 沿原查回预算读取 → 用已入库的接续消息号标记证据。
std::optional<agent::DecisionStatusSource> MainMessages::resumed_task_source(std::string_view reference, std::uint64_t message) const {
    const auto prefix = store_->conversation()+"/task/";
    if (!reference.starts_with(prefix) || reference.find('/',prefix.size())!=reference.npos) return {};
    try {
        const auto selection = store_->search_tasks({std::string(reference)},1,0);
        if (!selection.at("items").empty()) return task_source(message,selection);
    } catch (const std::invalid_argument&) { return {}; }
    return {};
}

// 原任务概览或逐字回执只在本轮决策可见。
agent::DecisionStatusSource MainMessages::task_source(std::uint64_t fact, const Json& selection) const {
    static const auto schema = [] {
        Json fields = Json::object();
        for (const auto key : {"reference","taskRef","origin","goal","goalRef","tool","plan","expected","execution","effect","verification",
            "actionRef","receiptRef","baselineRef","readbackRef","firstAction","nextAction","reviewState","reviewReason","review",
            "kind","payload","result","runResult"})
            fields[key] = {{"type","string"}};
        for (const auto key : {"count","provider","feedback","nextOffset","utcMs"})
            fields[key] = {{"type","integer"}};
        Json work = Json::object();
        for (const auto key : {"candidate","invocation","step","attempt","confirmation"}) work[key] = {{"type","integer"}};
        for (const auto key : {"callId","name","error","fault"}) work[key] = {{"type","string"}};
        for (const auto key : {"accepted","dispatched","succeeded"}) work[key] = {{"type","boolean"}};
        fields["work"] = {{"type","object"},{"properties",work},{"additionalProperties",false}};
        const Json item{{"type","object"},{"properties",fields},{"additionalProperties",false}};
        const Json value{{"type","object"},{"properties",{{"items",{{"type","array"},{"items",item}}},{"limited",{{"type","boolean"}}}}},
            {"required",{"items","limited"}},{"additionalProperties",false}};
        return Json::array({{{"key","memory.tasks"},{"version",1},{"description","本体工具任务的计划、行动、回执和空闲复盘"},
            {"defaultAttention",{{"mode","explicit"}}},{"snapshotRead",false},{"valueSchema",value}}}).dump();
    }();
    return {"memory",schema,Json::array({{{"key","memory.tasks"},{"version",1},{"subject",store_->conversation()},
        {"fact",fact},{"value",selection}}}).dump()};
}
}
