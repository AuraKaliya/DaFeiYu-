#include "inbox.hpp"
namespace dfy {
Json InboxRouter::accept(const std::string& source, const std::string& key, Json input) {
    require(!key.empty() && key.size() <= 160, "INPUT_KEY_INVALID");
    require(input.at("text").is_string() && input.at("text").get<std::string>().size() <= 65536, "INPUT_TEXT_INVALID");
    require(input.at("actor") == profile_.data.at("owner"), "INPUT_ACTOR_DENIED");
    require(input.at("conversation").is_string(), "INPUT_CONVERSATION_INVALID");
    const auto identity = source + ":" + key;
    auto previous = store_.get("input", identity);
    if (!previous.is_null()) { require(previous.at("input") == input, "INPUT_KEY_CONFLICT"); return previous; }
    size_t pending = 0;
    for (const auto& item : store_.all("input")) if (item.at("state") == "accepted") ++pending;
    require(pending < profile_.data.at("limits").at("max_inbox").get<size_t>(), "INBOX_FULL");
    Json value{{"id", identity}, {"source", source}, {"key", key}, {"input", input}, {"state", "accepted"}, {"received_at", now_ms()}, {"run_id", nullptr}};
    store_.save("input", identity, value, "input.accepted", value); return value;
}
void InboxRouter::route() {
    for (auto input : store_.all("input")) {
        if (input.at("state") != "accepted") continue;
        try {
        const auto& payload = input.at("input");
        const auto activity = payload.value("activity", std::string{});
        if (activity.empty() && !behavior_.user_ready()) continue;
        if (!activity.empty()) {
            const auto state = store_.get("activity", activity);
            if (state.is_null() || !state.at("enabled").get<bool>() || !includes(state.at("definition").at("allowed_modes"), behavior_.state().at("mode"))) continue;
        }
        if (payload.contains("run_id")) {
            auto target = store_.get("run", payload.at("run_id"));
            require(!target.is_null() && target.at("actor") == payload.at("actor") && target.at("conversation") == payload.at("conversation"), "STEER_SCOPE_DENIED");
            store_.atomic([&] { (void)runs_.steer(target.at("id"), payload); input["state"] = "consumed"; input["run_id"] = target.at("id");
                store_.put("input", input.at("id"), input); store_.event("input.consumed", input, target.at("id")); });
            continue;
        }
        const auto id = identifier();
        auto grants = policy_.current();
        if (!activity.empty()) {
            const auto definition = store_.get("activity", activity).at("definition");
            Json actions = Json::array(); for (const auto& action : definition.at("actions")) if (includes(grants.at("actions"), action)) actions.push_back(action);
            grants["actions"] = actions;
        }
        Json run{{"id", id}, {"input_id", input.at("id")}, {"input", payload}, {"actor", payload.at("actor")}, {"conversation", payload.at("conversation")},
            {"activity", activity}, {"execution", "queued"}, {"phase", "decision"}, {"revision", 1}, {"intent_revision", 1},
            {"bindings", plugins_.snapshot(grants.at("actions"))}, {"grants", grants}, {"limits", profile_.data.at("limits")},
            {"foundation", profile_.foundation}, {"persona", nullptr}, {"strategy_state", Json::object()}, {"strategy_steps", 0}, {"model_attempts", 0},
            {"results", Json::array()}, {"candidate", nullptr}, {"proposal", nullptr}, {"request", nullptr}, {"active_operation", nullptr}, {"wait_reason", nullptr},
            {"delivery_policy", payload.value("delivery_policy", std::string("reply"))}, {"delivery", "none"}, {"goal", "uncertain"}, {"goal_origin", "none"},
            {"created_at", now_ms()}, {"trigger", "run_ready"}, {"auto_depth", payload.value("auto_depth", 0)}, {"error", nullptr}};
        store_.atomic([&] {
            input["state"] = "consumed"; input["run_id"] = id;
            store_.put("input", input.at("id"), input); store_.put("run", id, run);
            store_.event("input.consumed", input, id); store_.event("run.accepted", run, id);
        });
        } catch (const std::exception& error) {
            input["state"] = "rejected"; input["error"] = failure(error);
            store_.save("input", input.at("id"), input, "input.rejected", input);
        }
    }
}
}
