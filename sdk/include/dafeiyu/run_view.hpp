#pragma once
#include <dafeiyu/value.hpp>

namespace dfy::sdk {
// A public decision view, never a storage record or authority token.
struct RunView {
    static constexpr int schema_version = 2;
    std::string id, execution, phase, goal, delivery;
    int revision{}, intent_revision{};
    Json input, conversation_key, candidate, results, persona, admitted_actions, budget;
    Json to_json() const {
        return {{"schema_version", schema_version}, {"id", id}, {"revision", revision}, {"intent_revision", intent_revision},
            {"execution", execution}, {"phase", phase}, {"goal", goal}, {"delivery", delivery}, {"input", input},
            {"conversation_key", conversation_key}, {"candidate", candidate}, {"results", results}, {"persona", persona}, {"admitted_actions", admitted_actions}, {"budget", budget}};
    }
    static RunView from_json(const Json& value) {
        if (value.at("schema_version") != schema_version) throw std::runtime_error("RUN_VIEW_VERSION_UNSUPPORTED");
        RunView view;
        view.id = value.at("id"); view.revision = value.at("revision"); view.intent_revision = value.at("intent_revision");
        view.execution = value.at("execution"); view.phase = value.at("phase"); view.goal = value.at("goal"); view.delivery = value.at("delivery");
        view.input = value.at("input"); view.conversation_key = value.at("conversation_key"); view.candidate = value.at("candidate"); view.results = value.at("results");
        view.persona = value.at("persona"); view.admitted_actions = value.at("admitted_actions"); view.budget = value.at("budget");
        return view;
    }
};
}
