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
        definitions_[id] = definition;
        behavior_.ensure_activity(id, definition.at("enabled"));
        auto value = store_.get("activity", id);
        if (value.is_null()) {
            value = {{"id", id}, {"definition", definition}, {"state", "registered"},
                {"next_due", now_ms() + definition.at("interval_ms").get<int64_t>()}, {"cursor", store_.sequence()}};
            writer_.save("activity", id, value, "activity.registered", value);
        } else { require(value.at("definition") == definition, "ACTIVITY_DEFINITION_CHANGED");
            if (value.at("state") != "registered") { value["state"] = "registered"; writer_.save("activity", id, value, "activity.restored"); } }
    }
    for (auto value : store_.all("activity")) if (!definitions_.contains(value.at("id").get<std::string>()) && value.at("state") != "retired") {
        value["state"] = "retired"; value["next_due"] = 0; writer_.save("activity", value.at("id"), value, "activity.retired");
    }
}
void Scheduler::trigger(Json activity, const std::string& key, const Json& fact) {
    const auto id = activity.at("id").get<std::string>();
    if (evaluating_.contains(id)) return;
    if (store_.active_activity_work(id)) return;
    const auto& definition = activity.at("definition");
    const auto occurrence_id = id + ":" + key;
    auto occurrence = store_.get("occurrence", occurrence_id);
    if (occurrence.is_null()) {
        occurrence = {{"id", occurrence_id}, {"activity", id}, {"key", key}, {"fact", fact}, {"state", "pending"}};
        writer_.save("occurrence", occurrence_id, occurrence, "activity.triggered", occurrence);
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
                if (!result.error.is_null()) {
                    auto value = store_.get("occurrence", occurrence_id); value["retry_at"] = now_ms() + definition.at("interval_ms").get<int64_t>();
                    writer_.save("occurrence", occurrence_id, value, "activity.trigger_deferred", result.error); return;
                }
                if (!behavior_.activity_policy(id).at("enabled").get<bool>() || !includes(definition.at("allowed_modes"), behavior_.state().at("mode"))) return;
                auto occurrence_value = store_.get("occurrence", occurrence_id);
                const auto decision = result.result.at("decision").get<std::string>();
                if (decision == "defer") {
                    occurrence_value["retry_at"] = now_ms() + definition.at("interval_ms").get<int64_t>();
                    writer_.save("occurrence", occurrence_id, occurrence_value, "activity.trigger_deferred"); return;
                }
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
                    writer_.put("occurrence", occurrence_id, occurrence_value); writer_.event("activity.evaluated", occurrence_value);
                });
            });
    } catch (const std::exception&) { evaluating_.erase(id); }
}
Json Scheduler::activity_views() {
    Json result = Json::array();
    for (const auto& [id, definition] : definitions_) {
        (void)definition; auto value = store_.get("activity", id); const auto policy = behavior_.activity_policy(id);
        value["enabled"] = policy.at("enabled"); value["policy_revision"] = policy.at("policy_revision"); result.push_back(std::move(value));
    }
    return result;
}
int64_t Scheduler::next_wakeup() {
    auto next = store_.next_wakeup();
    const auto expiry = behavior_.state().at("effective_until").get<int64_t>();
    if (expiry > 0 && (next == 0 || expiry < next)) next = expiry;
    return next;
}
void Scheduler::tick() {
    behavior_.tick();
    Json allowed = Json::array();
    const auto mode = behavior_.state().at("mode");
    for (const auto& [id, definition] : definitions_)
        if (behavior_.activity_policy(id).at("enabled") == true && includes(definition.at("allowed_modes"), mode)) allowed.push_back(id);
    for (auto activity : store_.due_activities(now_ms())) {
        const auto& definition = activity.at("definition");
        const auto interval = definition.at("interval_ms").get<int64_t>(), due = activity.at("next_due").get<int64_t>();
        activity["next_due"] = now_ms() + interval;
        if (definition.at("misfire") == "skip" && now_ms() - due >= interval) {
            writer_.save("activity", activity.at("id"), activity, "activity.missed"); continue;
        }
        if (store_.pending_activity_occurrence(activity.at("id"))) {
            writer_.save("activity", activity.at("id"), activity, "activity.coalesced"); continue;
        }
        const auto occurrence_id = activity.at("id").get<std::string>() + ":" + std::to_string(due);
        Json occurrence{{"id", occurrence_id}, {"activity", activity.at("id")}, {"key", std::to_string(due)}, {"fact", {{"due", due}}}, {"state", "pending"}};
        store_.atomic([&] { writer_.put("activity", activity.at("id"), activity); writer_.put("occurrence", occurrence_id, occurrence); writer_.event("activity.due", occurrence); });
    }
    for (const auto& id : allowed) {
        const auto pending = store_.pending_occurrences(id);
        if (!pending.empty()) trigger(store_.get("activity", id), pending[0].at("key"), pending[0].at("fact"));
    }
    inbox_.route();
    for (const auto& run : store_.ready_runs(now_ms(), 64, 1)) if (runs_.runnable(run)) runs_.advance(run);
    if (!runs_.busy()) {
        auto ready = behavior_.user_ready() ? store_.ready_runs(now_ms(), 64, 2) : Json::array();
        if (ready.empty()) ready = store_.ready_runs(now_ms(), 64, 3, allowed);
        for (const auto& run : ready) if (runs_.runnable(run)) { runs_.advance(run); break; }
    }
    for (const auto& run : store_.activity_feedback()) {
        const auto id = run.at("id").get<std::string>();
        if (notifying_.contains(id)) continue;
        if (!definitions_.contains(run.at("activity").get<std::string>())) { runs_.feedback_completed(id, {{"status", "provider-retired"}}); continue; }
        const auto activity = store_.get("activity", run.at("activity"));
        notifying_.insert(id);
        try {
            calls_.request(plugins_.binding(activity.at("definition").at("binding")), "on_settlement",
                {{"activity_id", run.at("activity")}, {"run", runs_.view(id)}}, run_scope(run, "activity_feedback"),
                profile_.data.at("limits").at("call_timeout_ms"), [this, id](const Completion& result) {
                    if (result.late) return;
                    notifying_.erase(id);
                    runs_.feedback_completed(id, result.error.is_null() ? result.result : Json{{"error", result.error}});
                });
        } catch (const std::exception& error) { notifying_.erase(id); runs_.feedback_completed(id, {{"error", failure(error)}}); }
    }
}
}
