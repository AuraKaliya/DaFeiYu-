#pragma once
#include <dafeiyu/value.hpp>

namespace dfy::sdk {
// 公开决策视图仅提供观察数据，不代表内部存储记录或授权令牌。
struct RunView {
    static constexpr int schema_version = 3;
    std::string id, execution, phase, goal, delivery;
    int revision{}, intent_revision{};
    Json input, conversation_key, candidate, results, persona, admitted_actions, budget;
    Json observation, model_bindings, error, wait_reason, active_operation;
    std::string activity_id, goal_origin;

    // 按当前公开视图版本序列化运行任务字段。
    Json to_json() const {
        return {{"schema_version", schema_version}, {"id", id}, {"revision", revision},
            {"intent_revision", intent_revision}, {"execution", execution}, {"phase", phase}, {"goal", goal},
            {"delivery", delivery}, {"input", input}, {"conversation_key", conversation_key}, {"candidate", candidate},
            {"results", results}, {"persona", persona}, {"admitted_actions", admitted_actions}, {"budget", budget},
            {"observation", observation}, {"model_bindings", model_bindings}, {"error", error},
            {"wait_reason", wait_reason}, {"active_operation", active_operation}, {"activity_id", activity_id},
            {"goal_origin", goal_origin}};
    }

    // 校验公开视图版本，并还原策略可读取的运行任务视图。
    // 步骤：检查版本 → 读取身份与执行状态 → 还原输入、结果、预算及观察字段。
    static RunView from_json(const Json& value) {
        if (value.at("schema_version") != schema_version)
            throw std::runtime_error("RUN_VIEW_VERSION_UNSUPPORTED");

        RunView view;
        view.id = value.at("id");
        view.revision = value.at("revision");
        view.intent_revision = value.at("intent_revision");
        view.execution = value.at("execution");
        view.phase = value.at("phase");
        view.goal = value.at("goal");
        view.delivery = value.at("delivery");
        view.input = value.at("input");
        view.conversation_key = value.at("conversation_key");
        view.candidate = value.at("candidate");
        view.results = value.at("results");
        view.persona = value.at("persona");
        view.admitted_actions = value.at("admitted_actions");
        view.budget = value.at("budget");
        view.observation = value.at("observation");
        view.model_bindings = value.at("model_bindings");
        view.error = value.at("error");
        view.wait_reason = value.at("wait_reason");
        view.active_operation = value.at("active_operation");
        view.activity_id = value.at("activity_id");
        view.goal_origin = value.at("goal_origin");
        return view;
    }
};
}
