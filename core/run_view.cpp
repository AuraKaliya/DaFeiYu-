#include "run_view.hpp"
namespace dfy {
sdk::RunView make_run_view(const Json& state, const Json& call_budget) {
    sdk::RunView view;
    view.id = state.at("id"); view.revision = state.at("revision"); view.intent_revision = state.at("intent_revision");
    view.execution = state.at("execution"); view.phase = state.at("phase"); view.goal = state.at("goal"); view.delivery = state.at("delivery");
    view.input = {{"text", state.at("input").at("text")}, {"actor", state.at("actor")}, {"conversation", state.at("conversation")}};
    view.conversation_key = state.at("conversation_key");
    view.candidate = state.at("candidate"); view.results = state.at("results"); view.persona = state.at("persona");
    view.admitted_actions = state.at("grants").at("actions");
    view.observation = state.at("observation"); view.error = state.at("error"); view.wait_reason = state.at("wait_reason");
    view.active_operation = state.at("active_operation"); view.activity_id = state.at("activity"); view.goal_origin = state.at("goal_origin");
    view.model_bindings = Json::array();
    for (const auto& [name, service] : state.at("bindings").at("bindings").items()) { (void)service; if (name == "model" || name.starts_with("model.")) view.model_bindings.push_back(name); }
    view.budget = {{"model_attempts", {{"used", state.at("model_attempts")}, {"limit", state.at("limits").at("model_attempts")}}},
        {"strategy_steps", {{"used", state.at("strategy_steps")}, {"limit", state.at("limits").at("strategy_steps")}}},
        {"nested_calls", {{"used", call_budget.is_null() ? Json(0) : call_budget.at("used")}, {"limit", state.at("limits").at("nested_calls")}}}};
    return view;
}
}
