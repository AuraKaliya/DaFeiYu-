#include <dafeiyu/core.hpp>
#include <dafeiyu/action_schema.hpp>
#include <algorithm>

namespace dfy {
namespace {
bool contains(const Json& list, const Json& value) { return std::find(list.begin(), list.end(), value) != list.end(); }
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void validate_outcome(const Json& result) {
    require(result.is_object(), "ACTION_RESULT_INVALID");
    require(contains(Json::array({"succeeded", "partial", "failed", "cancelled", "unknown"}), result.at("status")), "ACTION_STATUS_INVALID");
    require(contains(Json::array({"none", "confirmed", "unknown"}), result.at("effects")), "ACTION_EFFECTS_INVALID");
    require(contains(Json::array({"complete", "partial", "unknown"}), result.at("coverage")), "ACTION_COVERAGE_INVALID");
    require(result.at("sources").is_array() && result.at("receipts").is_array() && result.contains("data"), "ACTION_RESULT_INVALID");
}
}
Profile Profile::open(const Path& file) {
    const auto absolute = std::filesystem::absolute(file);
    const auto data = read_json(absolute);
    require(data.at("schema_version") == 1, "PROFILE_VERSION_UNSUPPORTED");
    require(data.at("owner").is_string() && !data.at("owner").get<std::string>().empty(), "PROFILE_OWNER_REQUIRED");
    require(data.at("allowed_actions").is_array() && data.at("allowed_effects").is_array(), "PROFILE_POLICY_INVALID");
    const auto turns = data.at("max_turns").get<int>();
    require(turns >= 1 && turns <= 32, "PROFILE_TURN_BUDGET_INVALID");
    return {data, absolute.parent_path() / sdk::path_from_utf8(data.at("plugin_directory").get<std::string>()),
        read_text(absolute.parent_path() / sdk::path_from_utf8(data.at("foundation").get<std::string>()))};
}
Runtime::Runtime(Profile profile, Path directory) : profile_(std::move(profile)), directory_(std::move(directory)), store_(directory_) {
    plugins_.load(profile_.plugins, directory_ / "plugins", store_.instance_id(), profile_.data.at("plugins"));
    for (const auto* kind : {"model", "memory", "channel"}) {
        const auto descriptor = plugins_.describe(profile_.data.at(kind).get<std::string>());
        require(descriptor.at("kind") == kind, "PROFILE_SERVICE_KIND_MISMATCH");
    }
    for (const auto& action : profile_.data.at("allowed_actions"))
        require(plugins_.describe(action.get<std::string>()).at("kind") == "action", "PROFILE_ACTION_INVALID");
}
Json Runtime::bindings() const {
    return {{"owner", profile_.data.at("owner")}, {"model", profile_.data.at("model")}, {"memory", profile_.data.at("memory")},
        {"channel", profile_.data.at("channel")}, {"allowed_actions", profile_.data.at("allowed_actions")},
        {"allowed_effects", profile_.data.at("allowed_effects")}, {"max_turns", profile_.data.at("max_turns")}, {"services", plugins_.catalog()}};
}
Json Runtime::context(const Json& run, const std::string& operation) const {
    return {{"instance_id", store_.instance_id()}, {"run_id", run.at("run_id")}, {"operation_id", operation},
        {"intent_revision", run.at("intent_revision")}, {"scope", run.at("input").at("conversation")}, {"actor", run.at("input").at("actor")}};
}
void Runtime::commit(Json& run, const std::string& event, const Json& payload, const RunOptions& options) {
    store_.commit(run, event, payload);
    if (options.observer) {
        try { options.observer(event); }
        catch (...) { store_.commit(run, "observer.error", {{"event", event}, {"code", "OBSERVER_EXCEPTION"}}); }
    }
}
Json Runtime::submit(const std::string& request_id, const std::string& message, const RunOptions& options) {
    require(!request_id.empty() && request_id.size() <= 120 && message.size() <= 65536, "INPUT_LIMIT");
    if (auto previous = store_.find(request_id); !previous.is_null()) {
        require(previous.at("input").at("text") == message, "REQUEST_ID_CONFLICT");
        return previous;
    }
    const auto inbound = plugins_.one(profile_.data.at("channel"), "ingest",
        {{"request_id", request_id}, {"text", message}}, {{"instance_id", store_.instance_id()}}, options.stop);
    require(inbound.at("actor") == profile_.data.at("owner"), "INGRESS_ACTOR_DENIED");
    require(inbound.at("request_id") == request_id && inbound.at("text") == message && inbound.at("conversation").is_string(), "INGRESS_INVALID");
    return drive(store_.create(request_id, inbound, bindings(), profile_.foundation), options);
}
Json Runtime::resume(const std::string& request_id, const RunOptions& options) {
    auto run = store_.find(request_id);
    require(!run.is_null(), "REQUEST_NOT_FOUND");
    require(run.at("bindings") == bindings(), "RESUME_BINDINGS_CHANGED");
    require(run.at("foundation") == profile_.foundation, "RESUME_FOUNDATION_CHANGED");
    require(run.at("execution") != "cancelled", "CANCELLED_RUN_REQUIRES_NEW_REQUEST");
    return drive(std::move(run), options);
}
Json Runtime::drive(Json run, const RunOptions& options) {
    if (run.at("phase") == "done") return run;
    const auto model = profile_.data.at("model").get<std::string>();
    const auto memory = profile_.data.at("memory").get<std::string>();
    const auto channel = profile_.data.at("channel").get<std::string>();
    try {
        if (!run.at("memory").is_null()) {
            const auto valid = plugins_.one(memory, "validate", run.at("memory"), context(run, "memory.validate"), options.stop);
            require(valid.at("valid") == true, "MEMORY_SNAPSHOT_INVALID");
        }
        run["execution"] = "running";
        run["error"] = nullptr;
        while (run.at("phase") != "done") {
            if (options.stop.stop_requested()) throw sdk::Cancelled{};
            const auto phase = run.at("phase").get<std::string>();
            if (phase == "memory") {
                const auto snapshot = plugins_.one(memory, "snapshot", Json::object(), context(run, "memory.snapshot"), options.stop);
                require(snapshot.at("revision").is_number_integer() && snapshot.at("persona").is_string() && snapshot.at("sources").is_array(), "MEMORY_SNAPSHOT_INVALID");
                run["memory"] = snapshot;
                run["phase"] = "model";
                commit(run, "memory.snapshot", snapshot, options);
            } else if (phase == "model") {
                const int turn = run.at("turn").get<int>();
                require(turn < profile_.data.at("max_turns").get<int>(), "TURN_BUDGET_EXHAUSTED");
                Json allowed = Json::array();
                for (const auto& id : profile_.data.at("allowed_actions")) allowed.push_back(plugins_.describe(id));
                const Json request{{"foundation", run.at("foundation")}, {"memory", run.at("memory")}, {"input", run.at("input")},
                    {"results", run.at("results")}, {"actions", allowed}, {"turn", turn}};
                run["turn"] = turn + 1;
                commit(run, "model.request", request, options);
                const auto events = plugins_.call(model, "generate", request, context(run, "model." + std::to_string(turn)), options.stop);
                Json decision;
                for (const auto& event : events) {
                    const auto type = event.at("type").get<std::string>();
                    if (type == "tool_call" || type == "final") {
                        require(decision.is_null(), "MODEL_MULTIPLE_DECISIONS");
                        decision = event;
                    } else require(type == "text_delta" || type == "usage", "MODEL_EVENT_INVALID");
                }
                require(!decision.is_null(), "MODEL_INCOMPLETE");
                if (decision.at("type") == "tool_call") {
                    require(decision.at("action").is_string() && decision.at("arguments").is_object(), "MODEL_TOOL_CALL_INVALID");
                    const std::string operation = run.at("run_id").get<std::string>() + ":action:" + std::to_string(turn);
                    run["operations"].push_back({{"id", operation}, {"action", decision.at("action")}, {"arguments", decision.at("arguments")}, {"state", "prepared"}});
                    run["phase"] = "action";
                } else {
                    require(decision.at("text").is_string() && decision.at("text").get<std::string>().size() <= 65536, "MODEL_FINAL_INVALID");
                    const auto goal = decision.value("goal", std::string("uncertain"));
                    require(contains(Json::array({"met", "partial", "unmet", "uncertain"}), goal), "MODEL_GOAL_INVALID");
                    run["goal"] = goal;
                    run["goal_origin"] = decision.contains("goal") ? "agent" : "none";
                    run["answer"] = decision.at("text");
                    run["delivery_plan"] = {{"id", run.at("run_id").get<std::string>() + ":delivery:0"},
                        {"target", run.at("input").at("conversation")}, {"text", decision.at("text")}};
                    run["delivery"] = "prepared";
                    run["phase"] = "delivery";
                }
                commit(run, "model.response", events, options);
            } else if (phase == "action") {
                auto& operation = run["operations"].back();
                const auto id = operation.at("action").get<std::string>();
                require(contains(profile_.data.at("allowed_actions"), id), "ACTION_NOT_AUTHORIZED");
                const auto descriptor = plugins_.describe(id);
                const auto effects = descriptor.at("effects").get<std::string>();
                require(contains(profile_.data.at("allowed_effects"), effects), "ACTION_EFFECT_NOT_AUTHORIZED");
                sdk::validate_action_arguments(descriptor.at("input_schema"), operation.at("arguments"));
                if (operation.at("state") == "unknown" || (operation.at("state") == "running" && effects != "read")) {
                    operation["state"] = "unknown";
                    run["execution"] = "waiting";
                    run["error"] = "ACTION_EFFECT_UNKNOWN";
                    commit(run, "operation.unknown", operation, options);
                    return run;
                }
                const auto readiness = plugins_.one(id, "readiness", Json::object(), context(run, operation.at("id")), options.stop);
                require(readiness.at("available") == true, "ACTION_NOT_READY");
                operation["state"] = "running";
                commit(run, "operation.started", operation, options);
                const auto outcome = plugins_.one(id, "execute", operation.at("arguments"), context(run, operation.at("id")), options.stop);
                validate_outcome(outcome);
                operation["result"] = outcome;
                operation["state"] = "settled";
                run["results"].push_back({{"operation_id", operation.at("id")}, {"action", id}, {"outcome", outcome}});
                run["phase"] = "model";
                commit(run, "operation.settled", operation, options);
                if (options.pause_after_action) {
                    run["execution"] = "waiting";
                    commit(run, "run.checkpoint", {{"reason", "pause-after-action"}}, options);
                    return run;
                }
            } else if (phase == "delivery") {
                const auto& plan = run.at("delivery_plan");
                require(plan.at("target") == run.at("input").at("conversation") && run.at("input").at("actor") == profile_.data.at("owner"), "DELIVERY_TARGET_DENIED");
                if (run.at("delivery") == "not-sent") {
                    run["execution"] = "waiting";
                    run["error"] = "DELIVERY_REQUIRES_RECONCILIATION";
                    commit(run, "delivery.waiting", {{"reason", "explicit-retry-not-implemented"}}, options);
                    return run;
                }
                Json result;
                if (run.at("delivery") == "sending" || run.at("delivery") == "unknown") {
                    result = plugins_.one(channel, "query_delivery", {{"id", plan.at("id")}}, context(run, plan.at("id")), options.stop);
                    require(result.at("id") == plan.at("id"), "DELIVERY_RECEIPT_MISMATCH");
                    // Absence of a receipt is not proof of non-delivery.
                    if (result.at("status") != "sent") {
                        run["delivery"] = "unknown";
                        run["execution"] = "waiting";
                        run["error"] = "DELIVERY_REQUIRES_RECONCILIATION";
                        commit(run, "delivery.unknown", result, options);
                        return run;
                    }
                } else {
                    require(run.at("delivery") == "prepared", "DELIVERY_STATE_INVALID");
                    run["delivery"] = "sending";
                    commit(run, "delivery.started", plan, options);
                    result = plugins_.one(channel, "deliver", plan, context(run, plan.at("id")), options.stop);
                }
                require(result.at("id") == plan.at("id"), "DELIVERY_RECEIPT_MISMATCH");
                const auto state = result.at("status").get<std::string>();
                require(state == "sent" || state == "not-sent" || state == "unknown", "DELIVERY_RESULT_INVALID");
                if (state == "sent") {
                    require(result.at("receipt_id").is_string() && !result.at("receipt_id").get<std::string>().empty(), "DELIVERY_RECEIPT_MISSING");
                    run["delivery"] = "sent";
                    run["execution"] = "completed";
                    run["phase"] = "done";
                } else {
                    run["delivery"] = state;
                    run["execution"] = "waiting";
                    run["error"] = "DELIVERY_REQUIRES_RECONCILIATION";
                }
                run["delivery_receipt"] = result;
                commit(run, "delivery.settled", result, options);
                return run;
            } else throw std::runtime_error("RUN_PHASE_INVALID");
        }
    } catch (const sdk::Cancelled&) {
        if (run.at("delivery") == "sending") run["delivery"] = "unknown";
        run["execution"] = "cancelled";
        run["error"] = "CANCELLED";
        store_.commit(run, "run.cancelled");
    } catch (const std::exception& error) {
        if (run.at("delivery") == "sending") run["delivery"] = "unknown";
        run["execution"] = "failed";
        run["error"] = std::string(error.what()).substr(0, 2048);
        store_.commit(run, "run.failed", {{"error", run.at("error")}});
    }
    return run;
}
}
