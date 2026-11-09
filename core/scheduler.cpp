#include "inbox.hpp"
namespace dfy {
void Scheduler::start() {
    for (const auto& definition : profile_.data.value("activities", Json::array())) {
        const auto id = definition.at("id").get<std::string>();
        require(definition.at("interval_ms").get<int64_t>() > 0 && definition.at("actions").is_array(), "ACTIVITY_DEFINITION_INVALID");
        require(definition.at("delivery_policy") == "none" || definition.at("delivery_policy") == "reply", "ACTIVITY_DELIVERY_INVALID");
        if (definition.at("delivery_policy") == "reply") (void)plugins_.channel_binding(plugins_.binding(definition.at("reply_channel")));
        require(includes(Json::array({"skip", "coalesce"}), definition.at("misfire")), "ACTIVITY_MISFIRE_UNSUPPORTED");
        const auto service = plugins_.service(plugins_.binding(definition.at("binding")));
        require(service.at("contract") == "dafeiyu.activity", "ACTIVITY_CONTRACT_INVALID");
        bool registered = false;
        for (const auto& registration : plugins_.activities())
            if (registration.at("id") == id && registration.at("service") == service.at("id")) registered = true;
        require(registered, "ACTIVITY_NOT_REGISTERED");
        auto value = store_.get("activity", id);
        if (value.is_null()) {
            value = {{"id", id}, {"definition", definition}, {"enabled", definition.at("enabled")}, {"policy_revision", 1},
                {"next_due", now_ms() + definition.at("interval_ms").get<int64_t>()}, {"cursor", store_.sequence()}};
            store_.save("activity", id, value, "activity.registered", value);
        } else require(value.at("definition") == definition, "ACTIVITY_DEFINITION_CHANGED");
    }
}
void Scheduler::trigger(Json activity, const std::string& key, const Json& fact) {
    const auto id = activity.at("id").get<std::string>();
    if (evaluating_.contains(id)) return;
    for (const auto& input : store_.all("input")) if (input.at("state") == "accepted" && input.at("input").value("activity", std::string{}) == id) return;
    for (const auto& run : store_.all("run")) if (run.at("activity") == id && !terminal(run)) return;
    const auto& definition = activity.at("definition");
    const auto occurrence_id = id + ":" + key;
    auto occurrence = store_.get("occurrence", occurrence_id);
    if (occurrence.is_null()) {
        occurrence = {{"id", occurrence_id}, {"activity", id}, {"key", key}, {"fact", fact}, {"state", "pending"}};
        store_.save("occurrence", occurrence_id, occurrence, "activity.triggered", occurrence);
    }
    if (occurrence.at("state") != "pending") return;
    evaluating_.insert(id);
    CallScope scope; scope.role = "activity"; scope.bindings = profile_.data.at("bindings");
    scope.grants = store_.get("system", "policy");
    scope.nested_limit = profile_.data.at("limits").at("nested_calls");
    try {
        calls_.request(plugins_.binding(definition.at("binding")), "evaluate_trigger",
            {{"occurrence_id", occurrence_id}, {"fact", fact}, {"behavior", behavior_.state()}}, scope,
            profile_.data.at("limits").at("call_timeout_ms"),
            [this, id, occurrence_id, definition](const Completion& result) {
                if (result.late) return;
                evaluating_.erase(id);
                if (!result.error.is_null()) return;
                auto current = store_.get("activity", id);
                if (!current.at("enabled").get<bool>() || !includes(definition.at("allowed_modes"), behavior_.state().at("mode"))) return;
                auto occurrence_value = store_.get("occurrence", occurrence_id);
                const auto decision = result.result.at("decision").get<std::string>();
                if (decision == "defer") return;
                require(decision == "skip" || decision == "propose", "ACTIVITY_DECISION_INVALID");
                store_.atomic([&] {
                    if (decision == "propose") {
                        Json input{{"text", result.result.at("text")}, {"actor", profile_.data.at("owner")}, {"conversation", definition.at("conversation")},
                            {"activity", id}, {"delivery_policy", definition.at("delivery_policy")}, {"auto_depth", 1}};
                        const auto reply_service = definition.at("delivery_policy") == "reply" ? plugins_.binding(definition.at("reply_channel")) : std::string{};
                        const auto receipt = inbox_.accept_activity(id, occurrence_id, input, reply_service);
                        occurrence_value["input_id"] = receipt.at("id");
                    }
                    occurrence_value["state"] = decision;
                    store_.put("occurrence", occurrence_id, occurrence_value); store_.event("activity.evaluated", occurrence_value);
                });
            });
    } catch (const std::exception&) { evaluating_.erase(id); }
}
void Scheduler::tick() {
    behavior_.tick();
    for (auto activity : store_.all("activity")) {
        const auto& definition = activity.at("definition");
        const auto interval = definition.at("interval_ms").get<int64_t>();
        if (now_ms() >= activity.at("next_due").get<int64_t>()) {
            const auto due = activity.at("next_due").get<int64_t>();
            if (definition.at("misfire") == "skip" && now_ms() - due >= interval) {
                activity["next_due"] = now_ms() + interval; store_.save("activity", activity.at("id"), activity, "activity.missed");
            } else {
                bool pending = false;
                for (const auto& occurrence : store_.all("occurrence"))
                    if (occurrence.at("activity") == activity.at("id") && occurrence.at("state") == "pending") pending = true;
                if (pending) {
                    activity["next_due"] = now_ms() + interval;
                    store_.save("activity", activity.at("id"), activity, "activity.coalesced");
                } else {
                const auto occurrence_id = activity.at("id").get<std::string>() + ":" + std::to_string(due);
                Json occurrence{{"id", occurrence_id}, {"activity", activity.at("id")}, {"key", std::to_string(due)}, {"fact", {{"due", due}}}, {"state", "pending"}};
                activity["next_due"] = now_ms() + interval;
                store_.atomic([&] { store_.put("activity", activity.at("id"), activity); store_.put("occurrence", occurrence_id, occurrence); store_.event("activity.due", occurrence); });
                }
            }
        }
        if (activity.at("enabled").get<bool>() && includes(definition.at("allowed_modes"), behavior_.state().at("mode"))) {
            for (const auto& occurrence : store_.all("occurrence")) {
                if (occurrence.at("activity") == activity.at("id") && occurrence.at("state") == "pending") {
                    trigger(activity, occurrence.at("key"), occurrence.at("fact")); break;
                }
            }
        }
    }
    inbox_.route();
    auto candidates = store_.all("run");
    // Already-issued operations settle regardless of behavior mode; only new decisions are gated.
    for (const auto& run : candidates) if (runs_.runnable(run) && (run.at("phase") == "action" || run.at("phase") == "delivery")) runs_.advance(run);
    if (!runs_.busy()) {
        for (bool activity : {false, true}) {
            bool dispatched = false;
            for (const auto& run : store_.all("run")) {
                if (!runs_.runnable(run) || (run.at("activity") != "") != activity || run.at("phase") == "action" || run.at("phase") == "delivery") continue;
                if (!activity && !behavior_.user_ready()) continue;
                if (activity) {
                    const auto definition = store_.get("activity", run.at("activity"));
                    if (!definition.at("enabled").get<bool>() || !includes(definition.at("definition").at("allowed_modes"), behavior_.state().at("mode"))) continue;
                }
                runs_.advance(run); dispatched = true; break;
            }
            if (dispatched) break;
        }
    }
    for (auto run : store_.all("run")) {
        if (!terminal(run) || run.at("activity") == "" || run.value("activity_notified", false) || notifying_.contains(run.at("id").get<std::string>())) continue;
        const auto activity = store_.get("activity", run.at("activity"));
        notifying_.insert(run.at("id").get<std::string>());
        try {
            calls_.request(plugins_.binding(activity.at("definition").at("binding")), "on_settlement", {{"run", run}}, run_scope(run, "activity_feedback"),
                profile_.data.at("limits").at("call_timeout_ms"), [this, id = run.at("id").get<std::string>()](const Completion& result) {
                    if (result.late) return;
                    notifying_.erase(id);
                    if (!result.error.is_null()) return;
                    auto current = store_.get("run", id); current["activity_notified"] = true;
                    store_.save("run", id, current, "activity.settled", result.result, id);
                });
        } catch (const std::exception&) { notifying_.erase(run.at("id").get<std::string>()); }
    }
}
}
