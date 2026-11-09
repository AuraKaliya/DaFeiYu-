#include "run.hpp"
namespace dfy {
RunExecutor::RunExecutor(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy,
    OperationCoordinator& operations, ContextAssembler& context, const Profile& profile)
    : store_(store), plugins_(plugins), calls_(calls), policy_(policy), operations_(operations), context_(context),
      timeout_(profile.data.at("limits").at("call_timeout_ms")) {}
void RunExecutor::save(Json run, const std::string& event, const Json& payload) {
    run["revision"] = run.at("revision").get<int>() + 1;
    const auto id = run.at("id").get<std::string>();
    store_.save("run", id, run, event, payload, id);
}
void RunExecutor::fail(const std::string& id, const Json& error, bool wait) {
    auto run = store_.get("run", id);
    if (terminal(run)) return;
    run["execution"] = wait ? "waiting" : "failed"; run["error"] = error;
    run["wait_reason"] = wait ? Json{{"type", "interrupted"}} : Json{};
    save(run, wait ? "run.waiting" : "run.failed", error);
}
bool RunExecutor::current(const std::string& id, int intent) const {
    auto run = store_.get("run", id); return !terminal(run) && run.at("intent_revision") == intent;
}
void RunExecutor::adopt(Json run, const Json& result, const std::string& decision) {
    const auto proposal = result.at("proposal");
    require(includes(Json::array({"request_model", "invoke_action", "wait", "finish"}), proposal.at("type")), "STRATEGY_PROPOSAL_INVALID");
    require(result.at("state").is_object(), "STRATEGY_STATE_INVALID");
    run["strategy_state"] = result.at("state"); run["proposal"] = proposal;
    run["decision_id"] = decision; run["phase"] = "execute"; run["execution"] = "queued"; run["wait_reason"] = nullptr;
    store_.atomic([&] {
        store_.put("decision", decision, {{"id", decision}, {"run_id", run.at("id")}, {"result", result}, {"intent_revision", run.at("intent_revision")}});
        save(run, "strategy.adopted", {{"decision_id", decision}, {"result", result}});
    });
}
void RunExecutor::strategy(Json run, bool review) {
    const auto id = run.at("id").get<std::string>(); const int intent = run.at("intent_revision");
    require(run.at("strategy_steps").get<int>() < run.at("limits").at("strategy_steps").get<int>(), "STRATEGY_BUDGET_EXHAUSTED");
    run["strategy_steps"] = run.at("strategy_steps").get<int>() + 1;
    const auto decision = id + ":decision:" + std::to_string(run.at("strategy_steps").get<int>());
    run["phase"] = review ? "reviewing" : "deciding"; run["execution"] = "running";
    Json request{{"decision_id", decision}, {"trigger", run.value("trigger", std::string("ready"))}, {"run", run}, {"state", run.at("strategy_state")}};
    if (review) request["candidate"] = run.at("proposal");
    save(run, "strategy.request", request); busy_.insert(id);
    try {
        calls_.request(run.at("bindings").at("bindings").at("strategy"), review ? "review_finish" : "propose",
            request, run_scope(run, "strategy"), timeout_,
            [this, id, intent, decision, review](const Completion& result) {
                if (!result.late) busy_.erase(id);
                if (result.late || !current(id, intent)) return;
                if (!result.error.is_null()) { fail(id, result.error, result.error.value("category", std::string{}) == "deadline"); return; }
                try {
                    auto value = store_.get("run", id);
                    if (review && result.result.value("accept", false)) {
                        value["strategy_state"] = result.result.at("state");
                        value["phase"] = "delivery"; value["execution"] = "queued";
                        store_.atomic([&] { store_.put("decision", decision, {{"id", decision}, {"run_id", id}, {"result", result.result}});
                            save(value, "strategy.finish_accepted", result.result); });
                    } else {
                        if (review) require(result.result.at("proposal").at("type") != "finish", "REVIEW_RECURSIVE_FINISH");
                        adopt(value, result.result, decision);
                    }
                } catch (const std::exception& error) { fail(id, failure(error)); }
            });
    } catch (...) { busy_.erase(id); throw; }
}
void RunExecutor::model(Json run, const Json& proposal) {
    const auto id = run.at("id").get<std::string>(); const int intent = run.at("intent_revision");
    require(run.at("model_attempts").get<int>() < run.at("limits").at("model_attempts").get<int>(), "MODEL_BUDGET_EXHAUSTED");
    run["phase"] = "context"; run["execution"] = "running"; save(run, "context.started"); busy_.insert(id);
    try {
        context_.assemble(run, proposal, [this, id, intent](const Completion& assembled) {
            if (!current(id, intent)) { busy_.erase(id); return; }
            if (!assembled.error.is_null()) { busy_.erase(id); fail(id, assembled.error, true); return; }
            try {
                auto value = store_.get("run", id); value["persona"] = assembled.result.at("persona");
                require(value.at("persona").at("instance_id") == store_.instance_id(), "PERSONA_INSTANCE_MISMATCH");
                value["model_attempts"] = value.at("model_attempts").get<int>() + 1;
                const auto attempt = id + ":model:" + std::to_string(value.at("model_attempts").get<int>());
                value["phase"] = "model"; value["request"] = assembled.result; value["attempt_id"] = attempt;
                store_.atomic([&] { store_.put("attempt", attempt, {{"id", attempt}, {"run_id", id}, {"request", assembled.result}, {"state", "running"}});
                    save(value, "model.request", {{"attempt_id", attempt}, {"request", assembled.result}}); });
                calls_.request(value.at("bindings").at("bindings").at("model"), "generate", assembled.result, run_scope(value, "model"), timeout_,
                    [this, id, intent, attempt](const Completion& output) {
                        if (!output.late) busy_.erase(id);
                        auto attempt_record = store_.get("attempt", attempt);
                        attempt_record["state"] = output.error.is_null() ? "completed" : "failed";
                        attempt_record["result"] = output.result; attempt_record["error"] = output.error; attempt_record["late"] = output.late;
                        store_.save("attempt", attempt, attempt_record, "model.settled", attempt_record, id);
                        if (output.late || !current(id, intent)) return;
                        if (!output.error.is_null()) { fail(id, output.error, true); return; }
                        try {
                            Json candidate;
                            for (const auto& event : output.result.at("events")) {
                                const auto type = event.at("type").get<std::string>();
                                if (type == "tool_call" || type == "final") { require(candidate.is_null(), "MODEL_MULTIPLE_DECISIONS"); candidate = event; }
                                else require(type == "text_delta" || type == "usage", "MODEL_EVENT_INVALID");
                            }
                            require(!candidate.is_null(), "MODEL_INCOMPLETE");
                            auto latest = store_.get("run", id);
                            latest["candidate"] = candidate; latest["phase"] = "decision"; latest["execution"] = "queued"; latest["trigger"] = "model_settled";
                            save(latest, "model.candidate", candidate);
                        } catch (const std::exception& error) { fail(id, failure(error)); }
                    });
            } catch (const std::exception& error) { busy_.erase(id); fail(id, failure(error), true); }
        });
    } catch (...) { busy_.erase(id); throw; }
}
void RunExecutor::complete(Json run) {
    run["execution"] = "completed"; run["phase"] = "done"; run["wait_reason"] = nullptr;
    run["goal"] = run.at("proposal").value("goal", std::string("uncertain"));
    require(includes(Json::array({"met", "partial", "unmet", "uncertain"}), run.at("goal")), "GOAL_INVALID");
    run["goal_origin"] = "strategy";
    run["answer"] = run.at("proposal").value("text", std::string{});
    save(run, "run.completed", {{"goal", run.at("goal")}, {"delivery", run.at("delivery")}});
}
bool RunExecutor::runnable(const Json& run) {
    if (terminal(run) || busy_.contains(run.at("id").get<std::string>())) return false;
    if (run.at("execution") == "paused") return false;
    if (run.at("execution") != "waiting") return true;
    if (!run.at("active_operation").is_null()) {
        const auto op = store_.get("operation", run.at("active_operation"));
        return !op.is_null() && (op.at("state") == "settled" ||
            (op.at("state") == "unknown" && run.at("wait_reason").value("type", std::string{}) != "effects"));
    }
    const auto& reason = run.at("wait_reason");
    return reason.is_object() && reason.value("type", std::string{}) == "time" && now_ms() >= reason.at("until").get<int64_t>();
}
void RunExecutor::advance(Json run) {
    const auto id = run.at("id").get<std::string>();
    try {
        require(plugins_.matches(run.at("bindings")), "BINDING_CHANGED");
        const auto phase = run.at("phase").get<std::string>();
        if (phase == "decision") { strategy(run, false); return; }
        if (phase == "review") { strategy(run, true); return; }
        if (phase == "wait") { run["phase"] = "decision"; run["execution"] = "queued"; run["trigger"] = "wait_resolved"; save(run, "run.woken"); return; }
        if (phase == "execute") {
            const auto proposal = run.at("proposal"); const auto type = proposal.at("type").get<std::string>();
            if (type == "request_model") { model(run, proposal); return; }
            if (type == "invoke_action") {
                const auto action = proposal.at("action").get<std::string>();
                require(run.at("bindings").at("actions").contains(action), "ACTION_DIRECTORY_DENIED");
                if (run.at("candidate").is_object() && run.at("candidate").value("type", std::string{}) == "tool_call") {
                    bool visible = false;
                    for (const auto& item : run.at("request").at("actions")) if (item.at("id") == action) visible = true;
                    require(visible, "MODEL_ACTION_NOT_VISIBLE");
                }
                const auto operation = operations_.action(run, run.at("bindings").at("actions").at(action), proposal.at("arguments"),
                    "action:" + std::to_string(run.at("strategy_steps").get<int>()));
                run["active_operation"] = operation; run["phase"] = "action"; run["execution"] = "waiting";
                run["wait_reason"] = {{"type", "operation"}, {"id", operation}}; save(run, "run.operation_wait");
            } else if (type == "finish") {
                for (const auto& operation : store_.all("operation")) if (operation.at("run_id") == id)
                    require(operation.at("state") == "settled", "RUN_EFFECT_UNRESOLVED");
                run["phase"] = "review"; run["execution"] = "queued"; save(run, "run.finish_candidate", proposal);
            } else {
                const auto reason = proposal.at("condition");
                require(reason.at("type") == "time" || reason.at("type") == "user", "WAIT_CONDITION_UNSUPPORTED");
                if (reason.at("type") == "time") require(reason.at("until").get<int64_t>() > now_ms(), "WAIT_TIME_INVALID");
                run["phase"] = "wait"; run["execution"] = "waiting"; run["wait_reason"] = reason; save(run, "run.waiting", reason);
            }
        } else if (phase == "action") {
            auto operation = store_.get("operation", run.at("active_operation"));
            if (operation.at("state") == "unknown") {
                run["execution"] = "waiting"; run["wait_reason"] = {{"type", "effects"}, {"operation_id", operation.at("id")}};
                save(run, "run.effects_wait"); return;
            }
            if (operation.at("state") == "settled") {
                bool recorded = false;
                for (const auto& result : run.at("results")) if (result.at("operation_id") == operation.at("id")) recorded = true;
                if (!recorded) run["results"].push_back({{"operation_id", operation.at("id")}, {"intent_revision", operation.at("intent_revision")}, {"action", run.at("proposal").at("action")}, {"outcome", operation.at("result")}});
                run["active_operation"] = nullptr; run["phase"] = "decision"; run["execution"] = "queued"; run["trigger"] = "operation_settled"; run["candidate"] = nullptr;
                save(run, "run.operation_consumed", operation);
            }
        } else if (phase == "delivery") {
            if (run.at("delivery_policy") == "none") { complete(run); return; }
            if (run.at("active_operation").is_null()) {
                const Json plan{{"target", run.at("conversation")}, {"text", run.at("proposal").value("text", std::string{})}, {"intent_revision", run.at("intent_revision")}};
                run["active_operation"] = operations_.deliver(run, plan); run["execution"] = "waiting"; run["delivery"] = "sending";
                run["wait_reason"] = {{"type", "delivery"}}; save(run, "run.delivery_wait");
            } else {
                const auto operation = store_.get("operation", run.at("active_operation"));
                if (operation.at("state") == "unknown") {
                    run["delivery"] = "unknown"; run["execution"] = "waiting"; run["wait_reason"] = {{"type", "effects"}, {"operation_id", operation.at("id")}};
                    save(run, "run.delivery_unknown"); return;
                }
                if (operation.at("state") != "settled") return;
                if (operation.at("intent_revision") != run.at("intent_revision")) {
                    run["active_operation"] = nullptr; run["phase"] = "decision"; run["execution"] = "queued";
                    run["delivery"] = "none"; run["trigger"] = "input_revised"; run["candidate"] = nullptr;
                    save(run, "run.superseded_delivery", operation); return;
                }
                const auto data = operation.at("result").at("data");
                if (data.value("status", std::string{}) == "sent") { run["delivery"] = "sent"; complete(run); }
                else { run["delivery"] = "not-sent"; run["execution"] = "paused"; save(run, "run.delivery_unconfirmed", data); }
            }
        }
    } catch (const std::exception& error) {
        const std::string reason = error.what();
        fail(id, failure(error), reason == "BINDING_CHANGED" || reason.find("UNRESOLVED") != std::string::npos || reason.find("NOT_READY") != std::string::npos);
    }
}
void RunExecutor::recover() {
    for (auto run : store_.all("run")) {
        if (terminal(run)) continue;
        if (!plugins_.matches(run.at("bindings"))) { fail(run.at("id"), sdk::error("BINDING_CHANGED", "unavailable"), true); continue; }
        if (includes(Json::array({"context", "model", "deciding", "reviewing"}), run.at("phase"))) {
            run["phase"] = run.at("phase") == "reviewing" ? "review" : "decision"; run["execution"] = "queued";
            run["trigger"] = "recovered"; save(run, "run.recovered");
        } else if (!run.at("active_operation").is_null()) {
            auto operation = store_.get("operation", run.at("active_operation"));
            if (operation.at("state") == "prepared") {
                run["active_operation"] = nullptr; run["phase"] = run.at("phase") == "delivery" ? "delivery" : "execute";
                run["execution"] = "queued"; save(run, "run.recovered");
            }
        }
    }
}
Json RunExecutor::cancel(const std::string& id) {
    auto run = store_.get("run", id); require(!run.is_null(), "RUN_MISSING");
    if (!terminal(run)) { run["execution"] = "cancelled"; save(run, "run.cancelled"); calls_.cancel_run(id); operations_.cancel_jobs(id); }
    return store_.get("run", id);
}
Json RunExecutor::steer(const std::string& id, const Json& input) {
    auto run = store_.get("run", id); require(!run.is_null() && !terminal(run), "RUN_NOT_STEERABLE");
    calls_.cancel_run(id);
    run = store_.get("run", id);
    run["intent_revision"] = run.at("intent_revision").get<int>() + 1; run["input"] = input;
    run["candidate"] = nullptr; run["trigger"] = "input_revised";
    if (run.at("active_operation").is_null()) { run["phase"] = "decision"; run["execution"] = "queued"; }
    save(run, "run.steered", input); return store_.get("run", id);
}
Json RunExecutor::resume(const std::string& id) {
    auto run = store_.get("run", id); require(!run.is_null() && !terminal(run), "RUN_NOT_RESUMABLE");
    require(plugins_.matches(run.at("bindings")), "BINDING_CHANGED");
    require(!calls_.pending_for(id), "CALL_STILL_RUNNING");
    if (run.at("active_operation").is_null()) run["phase"] = "decision";
    run["execution"] = "queued"; save(run, "run.resumed"); return store_.get("run", id);
}
}
