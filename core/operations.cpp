#include "operations.hpp"
namespace dfy {
CallScope run_scope(const Json& run, std::string role, std::string operation) {
    CallScope scope;
    scope.run = run.at("id"); scope.operation = std::move(operation); scope.role = std::move(role);
    scope.intent = run.at("intent_revision"); scope.bindings = run.at("bindings").at("bindings"); scope.grants = run.at("grants");
    return scope;
}
OperationCoordinator::OperationCoordinator(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy, const Profile& profile)
    : store_(store), plugins_(plugins), calls_(calls), policy_(policy),
      timeout_(profile.data.at("limits").at("call_timeout_ms")), max_jobs_(profile.data.at("limits").at("max_jobs")) {}
void OperationCoordinator::blocked(const Json& descriptor, const std::string& except) {
    if (descriptor.at("effects") == "read") return;
    for (const auto& operation : store_.all("operation"))
        if (operation.at("id") != except && operation.at("state") == "unknown" &&
            operation.at("descriptor").at("resource") == descriptor.at("resource"))
            throw std::runtime_error("RESOURCE_EFFECT_UNRESOLVED");
}
Json OperationCoordinator::prepare(const Json& run, const Json& descriptor, const Json& input, const std::string& key, const std::string& kind, const std::string& parent) {
    require(!key.empty() && key.size() <= 160, "OPERATION_KEY_INVALID");
    const auto id = (parent.empty() ? run.at("id").get<std::string>() : parent) + ":" + key;
    auto operation = store_.get("operation", id);
    if (!operation.is_null()) {
        require(operation.at("input") == input && operation.at("descriptor") == descriptor && operation.at("kind") == kind, "OPERATION_PLAN_CONFLICT");
        return operation;
    }
    blocked(descriptor);
    operation = {{"id", id}, {"run_id", run.at("id")}, {"intent_revision", run.at("intent_revision")}, {"parent", parent},
        {"kind", kind}, {"descriptor", descriptor}, {"input", input}, {"state", "prepared"}, {"revision", 1}, {"result", nullptr}, {"job_id", nullptr}};
    store_.save("operation", id, operation, "operation.prepared", operation, run.at("id"));
    return operation;
}
void OperationCoordinator::dispatch(Json operation, CallScope scope, Done done) {
    if (operation.at("state") == "settled" || operation.at("state") == "unknown") { if (done) done(operation); return; }
    if (operation.at("state") != "prepared") return;
    const auto id = operation.at("id").get<std::string>();
    const auto descriptor = operation.at("descriptor");
    blocked(descriptor, id);
    scope.operation = id; scope.effects = descriptor.at("effects"); scope.resource = descriptor.at("resource");
    auto input = operation.at("input");
    // Operation identity is host metadata; domain input schemas describe only business parameters.
    operation["state"] = "running"; operation["revision"] = operation.at("revision").get<int>() + 1;
    store_.save("operation", id, operation, "operation.started", operation, operation.at("run_id"));
    try {
        calls_.request(descriptor.at("service"), descriptor.value("method", std::string("execute")), input, scope, timeout_,
            [this, id, done](const Completion& value) { settle(id, value, done); });
    } catch (const std::exception& error) {
        auto failed = store_.get("operation", id); failed["state"] = "prepared"; failed["error"] = failure(error);
        store_.save("operation", id, failed, "operation.not_dispatched", failed.at("error"), scope.run);
        if (done) done(failed);
    }
}
void OperationCoordinator::settle(const std::string& id, const Completion& completion, Done done) {
    auto operation = store_.get("operation", id);
    if (operation.is_null()) return;
    if (operation.at("state") == "settled") {
        store_.atomic([&] { store_.event("operation.additional_evidence", {{"result", completion.result}, {"error", completion.error}}, operation.at("run_id")); });
        if (done) done(operation);
        return;
    }
    const auto& descriptor = operation.at("descriptor");
    if (completion.error.is_null() && completion.result.is_object() && completion.result.value("type", std::string{}) == "job_started") {
        const auto job = store_.get("job", completion.result.at("job_id"));
        require(!job.is_null() && job.at("operation_id") == id, "JOB_HANDOFF_INVALID");
        if (job.at("state") != "completed") {
            operation["state"] = "job"; operation["job_id"] = job.at("id");
            store_.save("operation", id, operation, "operation.job_handoff", job, operation.at("run_id"));
        }
        if (done) done(store_.get("operation", id));
        return;
    }
    Json result;
    try {
        if (!completion.error.is_null()) {
            result = sdk::outcome({{"error", completion.error}}, descriptor.at("effects") == "read" ? "none" : "unknown", "failed");
        } else if (operation.at("kind") == "delivery") {
            const auto status = completion.result.at("status").get<std::string>();
            require(completion.result.at("id") == id && includes(Json::array({"sent", "not-sent", "unknown"}), status), "DELIVERY_RECEIPT_INVALID");
            if (status == "sent") require(!completion.result.at("receipt_id").get<std::string>().empty(), "DELIVERY_RECEIPT_MISSING");
            result = sdk::outcome(completion.result, status == "sent" ? "confirmed" : status == "not-sent" ? "none" : "unknown", status == "sent" ? "succeeded" : "unknown");
        } else {
            result = completion.result;
            validate_outcome(result);
        }
    } catch (const std::exception& error) {
        result = sdk::outcome({{"error", failure(error)}}, descriptor.at("effects") == "read" ? "none" : "unknown", "failed");
    }
    for (const auto& child : store_.all("operation"))
        if (child.at("parent") == id && child.at("state") != "settled") result["effects"] = "unknown";
    operation["state"] = result.at("effects") == "unknown" ? "unknown" : "settled";
    operation["result"] = result; operation["revision"] = operation.at("revision").get<int>() + 1;
    store_.save("operation", id, operation, operation.at("state") == "unknown" ? "operation.unknown" : "operation.settled", operation, operation.at("run_id"));
    if (done) done(operation);
}
std::string OperationCoordinator::action(const Json& run, const Json& descriptor, const Json& input, const std::string& key) {
    policy_.action(run, descriptor, input);
    auto operation = prepare(run, descriptor, input, key, "action");
    const auto id = operation.at("id").get<std::string>();
    auto scope = run_scope(run, "action", id);
    if (operation.at("state") == "prepared") {
        const auto methods = plugins_.service(descriptor.at("service")).at("methods");
        if (methods.contains("readiness")) {
            calls_.request(descriptor.at("service"), "readiness", {{"input", input}}, scope, timeout_,
                [this, operation, scope](const Completion& readiness) {
                    if (readiness.late) return;
                    if (!readiness.error.is_null() || !readiness.result.value("available", false)) {
                        Completion failed{{}, readiness.error.is_null() ? sdk::error("ACTION_UNAVAILABLE", "unavailable") : readiness.error};
                        auto value = store_.get("operation", operation.at("id"));
                        value["state"] = "settled"; value["result"] = sdk::outcome({{"error", failed.error}}, "none", "failed");
                        store_.save("operation", value.at("id"), value, "operation.unavailable", failed.error, scope.run); return;
                    }
                    auto current_run = store_.get("run", scope.run);
                    if (terminal(current_run) || current_run.at("intent_revision") != scope.intent) return;
                    try { policy_.action(current_run, operation.at("descriptor"), operation.at("input")); dispatch(operation, scope); }
                    catch (const std::exception& error) { settle(operation.at("id"), {{}, failure(error)}); }
                });
        } else dispatch(operation, scope);
    }
    return id;
}
std::string OperationCoordinator::deliver(const Json& run, const Json& plan) {
    policy_.delivery(run, plan);
    const auto service = run.at("bindings").at("bindings").at("channel").get<std::string>();
    auto descriptor = plugins_.method(service, "deliver");
    descriptor["service"] = service; descriptor["method"] = "deliver"; descriptor["query"] = "query_delivery";
    auto input = plan; input["id"] = run.at("id").get<std::string>() + ":delivery:" + std::to_string(run.at("intent_revision").get<int>());
    auto operation = prepare(run, descriptor, input, "delivery:" + std::to_string(run.at("intent_revision").get<int>()), "delivery");
    dispatch(operation, run_scope(run, "delivery", operation.at("id"))); return operation.at("id");
}
void OperationCoordinator::nested(const PluginMessage& message) {
    const auto parent = calls_.owned(message.plugin, message.body.at("call_id"));
    const auto name = message.body.at("binding").get<std::string>();
    require(plugins_.dependency(message.plugin, name), "DEPENDENCY_ACCESS_DENIED");
    require(parent.scope.bindings.contains(name), "RUN_BINDING_DENIED");
    const auto service = parent.scope.bindings.at(name).get<std::string>();
    const auto method = message.body.at("method").get<std::string>();
    auto descriptor = plugins_.method(service, method);
    require(plugins_.service(service).at("contract") != "dafeiyu.model", "MODEL_SCHEDULING_REQUIRED");
    policy_.method(parent.scope, descriptor);
    const auto reply = [this, source = message.plugin, key = message.body.at("key"), ticket = parent.id](const Completion& value) {
        if (value.late) return;
        plugins_.send(source, {{"type", "reply"}, {"key", key}, {"call_id", ticket}, {"result", value.result}, {"error", value.error}});
    };
    if (descriptor.at("effects") == "read") {
        calls_.request(service, method, message.body.at("input"), parent.scope, timeout_, reply);
    } else {
        descriptor["service"] = service; descriptor["method"] = method;
        auto run = store_.get("run", parent.scope.run);
        auto operation = prepare(run, descriptor, message.body.at("input"), message.body.at("key"), "service", parent.scope.operation);
        dispatch(operation, parent.scope, [reply](const Json& value) {
            if (value.at("state") == "settled" || value.at("state") == "unknown") reply({value.at("result"), {}});
            else reply({{}, sdk::error("SERVICE_NOT_DISPATCHED", "unavailable")});
        });
    }
}
void OperationCoordinator::register_job(const PluginMessage& message) {
    const auto parent = calls_.owned(message.plugin, message.body.at("call_id"));
    require(!parent.scope.operation.empty() && parent.scope.role == "action", "JOB_SCOPE_DENIED");
    auto operation = store_.get("operation", parent.scope.operation);
    require(operation.at("descriptor").at("service") == parent.service && operation.at("state") == "running", "JOB_OPERATION_OWNER_DENIED");
    const auto work = message.body.at("work_key").get<std::string>();
    require(!work.empty() && work.size() <= 128, "JOB_WORK_KEY_INVALID");
    const auto id = parent.scope.operation + ":job:" + fingerprint(work);
    auto job = store_.get("job", id);
    if (!job.is_null()) require(job.at("work_key") == work && job.at("plugin") == message.plugin, "JOB_REGISTRATION_CONFLICT");
    if (job.is_null()) {
        size_t count = 0; for (const auto& item : store_.all("job")) if (item.at("state") != "completed") ++count;
        require(count < static_cast<size_t>(max_jobs_), "JOB_LIMIT");
        require(operation.at("job_id").is_null(), "OPERATION_ALREADY_HAS_JOB");
        job = {{"id", id}, {"run_id", parent.scope.run}, {"operation_id", parent.scope.operation}, {"plugin", message.plugin},
            {"service", parent.service}, {"work_key", work}, {"state", "registered"}, {"sequence", 0}, {"result", nullptr}, {"cancel_requested", false}};
        operation["job_id"] = id;
        store_.atomic([&] { store_.put("job", id, job); store_.put("operation", parent.scope.operation, operation); store_.event("job.registered", job, parent.scope.run); });
    }
    plugins_.allow_job(message.plugin, id);
    plugins_.send(message.plugin, {{"type", "reply"}, {"call_id", parent.id}, {"key", message.body.at("key")}, {"result", job}, {"error", nullptr}});
}
void OperationCoordinator::job_result(const PluginMessage& message) {
    const auto id = message.body.at("job_id").get<std::string>(); auto job = store_.get("job", id);
    require(!job.is_null() && job.at("plugin") == message.plugin, "JOB_OWNER_DENIED");
    const auto sequence = message.body.at("sequence").get<uint64_t>(); require(sequence > 0, "JOB_SEQUENCE_INVALID");
    if (job.at("state") == "completed") require(job.at("sequence") == sequence && job.at("result") == message.body.at("result"), "JOB_TERMINAL_CONFLICT");
    else {
        validate_outcome(message.body.at("result"));
        job["state"] = "completed"; job["sequence"] = sequence; job["result"] = message.body.at("result");
        store_.atomic([&] {
            store_.put("job", id, job); settle(job.at("operation_id"), {job.at("result"), {}});
            store_.event("job.completed", job, job.at("run_id"));
        });
    }
    plugins_.send(message.plugin, {{"type", "job_receipt"}, {"job_id", id}, {"sequence", sequence}});
}
void OperationCoordinator::cancel_jobs(const std::string& run_id) {
    for (auto job : store_.all("job")) {
        if (job.at("run_id") != run_id || job.at("state") == "completed" || job.at("cancel_requested") == true) continue;
        job["cancel_requested"] = true; store_.save("job", job.at("id"), job, "job.cancel_requested", job, run_id);
        auto run = store_.get("run", run_id);
        try { calls_.request(job.at("service"), "cancel_job", job, run_scope(run, "job_control", job.at("operation_id")), timeout_, [](const Completion&) {}); }
        catch (const std::exception& error) { store_.atomic([&] { store_.event("job.cancel_unavailable", failure(error), run_id); }); }
    }
}
void OperationCoordinator::query(const std::string& id) {
    auto operation = store_.get("operation", id); require(!operation.is_null(), "OPERATION_MISSING");
    const auto& descriptor = operation.at("descriptor");
    const auto query_method = descriptor.value("query", std::string("query_operation"));
    auto run = store_.get("run", operation.at("run_id"));
    require(plugins_.matches(run.at("bindings")), "BINDING_CHANGED");
    calls_.request(descriptor.at("service"), query_method, {{"id", id}, {"input", operation.at("input")}}, run_scope(run, "reconcile", id), timeout_,
        [this, id](const Completion& result) { if (result.error.is_null()) settle(id, result); });
}
void OperationCoordinator::recover() {
    for (auto operation : store_.all("operation")) {
        if (operation.at("state") != "running" && operation.at("state") != "job") continue;
        if (!operation.at("job_id").is_null()) continue;
        operation["state"] = operation.at("descriptor").at("effects") == "read" ? "prepared" : "unknown";
        store_.save("operation", operation.at("id"), operation, "operation.interrupted", operation, operation.at("run_id"));
        if (operation.at("state") == "unknown") { try { query(operation.at("id")); } catch (const std::exception&) { /* Original binding may be unavailable. Keep unresolved. */ } }
    }
    for (auto job : store_.all("job")) {
        if (job.at("state") == "completed") continue;
        auto run = store_.get("run", job.at("run_id"));
        try {
            require(plugins_.matches(run.at("bindings")), "BINDING_CHANGED");
            plugins_.allow_job(job.at("plugin"), job.at("id"));
            calls_.request(job.at("service"), "query_job", job, run_scope(run, "job_recovery", job.at("operation_id")), timeout_,
                [this, job, run](const Completion& result) {
                    if (result.late) return;
                    if (result.error.is_null() && result.result.value("state", std::string{}) == "completed") {
                        job_result({job.at("plugin"), {{"job_id", job.at("id")}, {"sequence", 1}, {"result", result.result.at("result")}}});
                    } else if (result.error.is_null() && result.result.value("state", std::string{}) == "running") {
                        calls_.request(job.at("service"), "reattach_job", job, run_scope(run, "job_recovery", job.at("operation_id")), timeout_, [](const Completion&) {});
                    } else {
                        settle(job.at("operation_id"), {{}, sdk::error("JOB_INTERRUPTED", "unavailable", "unknown")});
                    }
                });
        } catch (const std::exception&) { /* Leave the registered work for explicit reconciliation. */ }
    }
}
Json OperationCoordinator::reconcile(const Json& command) {
    auto value = store_.get("operation", command.at("id"));
    require(!value.is_null() && value.at("state") == "unknown", "OPERATION_NOT_UNRESOLVED");
    require(value.at("revision") == command.at("expected_revision"), "OPERATION_REVISION_CONFLICT");
    require(!command.at("reason").get<std::string>().empty() && !command.at("evidence").get<std::string>().empty(), "RECONCILIATION_EVIDENCE_REQUIRED");
    validate_outcome(command.at("result")); require(command.at("result").at("effects") != "unknown", "RECONCILIATION_UNRESOLVED");
    store_.atomic([&] { store_.event("operation.reconciliation", command, value.at("run_id")); settle(value.at("id"), {command.at("result"), {}}); });
    return store_.get("operation", value.at("id"));
}
}
