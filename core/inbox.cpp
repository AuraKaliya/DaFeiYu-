#include "inbox.hpp"
namespace dfy {
Json InboxRouter::accept_local(const std::string& key, Json input, const std::string& channel) {
    input["actor"] = profile_.data.at("owner"); input["delivery_policy"] = "reply";
    const auto route = conversations_.local(input.at("conversation"), channel);
    return accept_bound("local", key, std::move(input), route);
}
Json InboxRouter::accept_channel(const std::string& plugin, const std::string& channel, const std::string& key, Json input) {
    const auto route = conversations_.channel(plugin, channel, input);
    input["actor"] = profile_.data.at("owner"); input["delivery_policy"] = "reply";
    input.erase("activity"); input.erase("auto_depth");
    const auto source = Json::array({plugin, channel, route.at("key").at("channel_instance"), route.at("key").at("account")}).dump();
    return accept_bound(source, key, std::move(input), route);
}
Json InboxRouter::accept_activity(const std::string& id, const std::string& key, Json input, const std::string& channel) {
    const auto route = conversations_.activity(id, input.at("conversation"), channel);
    return accept_bound("activity", key, std::move(input), route);
}
Json InboxRouter::steer_local(const std::string& key, const Json& run, const std::string& text) {
    const Json route{{"id", run.at("conversation")}, {"key", run.at("conversation_key")}, {"delivery", run.at("delivery_binding")}};
    return accept_bound("local", key, {{"run_id", run.at("id")}, {"text", text}, {"actor", profile_.data.at("owner")},
        {"conversation", run.at("external_conversation")}}, route);
}
Json InboxRouter::accept_bound(const std::string& source, const std::string& key, Json input, const Json& route) {
    require(!key.empty() && key.size() <= 160, "INPUT_KEY_INVALID");
    require(input.at("text").is_string() && input.at("text").get<std::string>().size() <= 65536, "INPUT_TEXT_INVALID");
    require(input.at("actor") == profile_.data.at("owner"), "INPUT_ACTOR_DENIED");
    require(input.at("conversation").is_string(), "INPUT_CONVERSATION_INVALID");
    const auto identity = source + ":" + key;
    auto previous = store_.get("input", identity);
    if (!previous.is_null()) { require(previous.at("input") == input && previous.at("route") == route, "INPUT_KEY_CONFLICT"); return previous; }
    size_t pending = 0;
    for (const auto& item : store_.all("input")) if (item.at("state") == "accepted") ++pending;
    require(pending < profile_.data.at("limits").at("max_inbox").get<size_t>(), "INBOX_FULL");
    Json value{{"id", identity}, {"source", source}, {"key", key}, {"input", input}, {"route", route}, {"state", "accepted"}, {"received_at", now_ms()}, {"run_id", nullptr}};
    store_.atomic([&] {
        if (store_.get("conversation", route.at("id")).is_null()) store_.put("conversation", route.at("id"), route.at("key"));
        store_.put("input", identity, value); store_.event("input.accepted", value);
    }); return value;
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
            require(!target.is_null() && target.at("actor") == payload.at("actor") && target.at("conversation_key") == input.at("route").at("key"), "STEER_SCOPE_DENIED");
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
        const auto& route = input.at("route");
        const std::string channel = route.at("delivery").is_null() ? "" : route.at("delivery").at("channel").at("service").get<std::string>();
        Json run{{"id", id}, {"input_id", input.at("id")}, {"input", payload}, {"actor", payload.at("actor")}, {"conversation", route.at("id")},
            {"conversation_key", route.at("key")}, {"external_conversation", payload.at("conversation")}, {"delivery_binding", route.at("delivery")},
            {"activity", activity}, {"execution", "queued"}, {"phase", "decision"}, {"revision", 1}, {"intent_revision", 1},
            {"bindings", plugins_.snapshot(grants.at("actions"), channel)}, {"grants", grants}, {"limits", profile_.data.at("limits")},
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
