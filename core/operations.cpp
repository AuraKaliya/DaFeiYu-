#include "operations.hpp"
#include "capabilities.hpp"

namespace dfy {
namespace {
// 将动作参数包装为携带定义版本和操作标识的执行请求。
Json invocation(const Json& descriptor, const Json& input, const std::string& operation) {
    return {{"action_id", descriptor.at("id")}, {"definition_revision", descriptor.at("revision")},
        {"arguments", input}, {"operation_id", operation}};
}

// 生成用于就绪检查或结果查询的只读描述符。
Json read_descriptor(Json descriptor) {
    descriptor["effects"] = "read";
    descriptor.erase("delegates");
    return descriptor;
}
}

// 从运行任务快照构造服务调用的身份、授权和预算上下文。
CallScope run_scope(const Json& run, std::string role, std::string operation) {
    CallScope scope;
    scope.run = run.at("id");
    scope.operation = std::move(operation);
    scope.role = std::move(role);
    scope.intent = run.at("intent_revision");
    scope.bindings = run.at("bindings").at("bindings");
    scope.grants = run.at("grants");
    scope.nested_limit = run.at("limits").at("nested_calls");
    return scope;
}

// 连接操作协调依赖，并领取操作记录的写权限。
OperationCoordinator::OperationCoordinator(StateStore& store, PluginRuntime& plugins, CallRouter& calls,
    PolicyGate& policy, Conversations& conversations, ScopeManager& scopes, const Profile& profile)
    : store_(store), writer_(store.claim("operations", {"operation"})), plugins_(plugins), calls_(calls),
      policy_(policy), conversations_(conversations), scopes_(scopes),
      timeout_(profile.data.at("limits").at("call_timeout_ms")) {
}

// 阻止向仍存在未知副作用的资源派发新的写操作。
void OperationCoordinator::blocked(const Json& descriptor, const std::string& except) {
    if (descriptor.at("effects") == "read")
        return;
    for (const auto& operation : store_.unknown_operations(resources_of(descriptor)))
        if (operation.at("id") != except && operation.at("state") == "unknown")
            for (const auto& resource : resources_of(descriptor))
                if (includes(resources_of(operation.at("descriptor")), resource))
                    throw std::runtime_error("RESOURCE_EFFECT_UNRESOLVED");
}

// 幂等准备操作记录，并阻止冲突计划或资源上未决副作用。
// 步骤：生成操作标识 → 校验并复用已有计划 → 检查资源阻塞 → 持久化准备状态。
Json OperationCoordinator::prepare(const Json& run, const Json& descriptor, const Json& input, const std::string& key,
    const std::string& kind, const std::string& parent) {
    require(!key.empty() && key.size() <= 160, "OPERATION_KEY_INVALID");
    const auto id = (parent.empty() ? run.at("id").get<std::string>() : parent) + ":" + key;

    auto operation = store_.get("operation", id);
    if (!operation.is_null()) {
        require(
            operation.at("input") == input && operation.at("descriptor") == descriptor && operation.at("kind") == kind,
            "OPERATION_PLAN_CONFLICT");
        return operation;
    }

    blocked(descriptor);
    operation = {{"id", id}, {"run_id", run.at("id")}, {"intent_revision", run.at("intent_revision")},
        {"parent", parent}, {"kind", kind}, {"descriptor", descriptor}, {"input", input}, {"state", "prepared"},
        {"revision", 1}, {"result", nullptr}, {"job_id", nullptr}};
    writer_.save("operation", id, operation, "operation.prepared", operation, run.at("id"));
    return operation;
}

// 将已准备操作派发给插件，并跟踪结算或未派发失败。
// 步骤：检查操作状态及资源阻塞 → 组装调用并保存运行状态 → 异步派发并在回调结算 → 保存派发失败结果。
void OperationCoordinator::dispatch(Json operation, CallScope scope, Done done) {
    if (operation.at("state") == "settled" || operation.at("state") == "unknown") {
        if (done)
            done(operation, {operation.at("result"), {}});
        return;
    }
    if (operation.at("state") != "prepared")
        return;

    const auto id = operation.at("id").get<std::string>();
    const auto descriptor = operation.at("descriptor");
    blocked(descriptor, id);
    scope.operation = id;
    scope.effects = descriptor.at("effects");
    scope.resource = descriptor.at("resource");

    auto input = operation.at("input");
    if (operation.at("kind") == "action") {
        input = invocation(descriptor, input, id);
        scope.action_definition = descriptor;
    }

    // 操作身份属于宿主元数据，领域输入契约只描述业务参数。
    operation["state"] = "running";
    operation["revision"] = operation.at("revision").get<int>() + 1;
    writer_.save("operation", id, operation, "operation.started", operation, operation.at("run_id"));

    try {
        calls_.request(descriptor.at("service"), descriptor.value("method", std::string("execute")), input, scope,
            timeout_, [this, id, done](const Completion& value) { settle(id, value, done); });
    } catch (const std::exception& error) {
        auto failed = store_.get("operation", id);
        failed["state"] = "settled";
        failed["error"] = failure(error);
        failed["result"] = sdk::outcome({{"error", failed.at("error")}}, "none", "failed");
        writer_.save("operation", id, failed, "operation.not_dispatched", failed.at("error"), scope.run);
        if (done)
            done(failed, {{}, failure(error)});
    }
}

// 归并操作完成证据，确定副作用已确认、未发生或仍待核对。
// 步骤：处理补充证据及作业交接 → 校验并标准化完成结果 → 合并子操作副作用 → 保存结算状态并回调。
void OperationCoordinator::settle(const std::string& id, const Completion& completion, Done done) {
    auto operation = store_.get("operation", id);
    if (operation.is_null())
        return;
    if (operation.at("state") == "settled") {
        store_.atomic([&] {
            writer_.event("operation.additional_evidence", {{"result", completion.result}, {"error", completion.error}},
                operation.at("run_id"));
        });
        if (done)
            done(operation, completion);
        return;
    }

    const auto& descriptor = operation.at("descriptor");
    if (completion.error.is_null() && completion.result.is_object() &&
        completion.result.value("type", std::string{}) == "job_started") {
        const auto job = store_.get("job", completion.result.at("job_id"));
        require(!job.is_null() && job.at("operation_id") == id, "JOB_HANDOFF_INVALID");
        if (job.at("state") != "completed") {
            operation["state"] = "job";
            operation["job_id"] = job.at("id");
            writer_.save("operation", id, operation, "operation.job_handoff", job, operation.at("run_id"));
        }
        if (done)
            done(store_.get("operation", id), completion);
        return;
    }

    Json result;
    try {
        if (!completion.error.is_null()) {
            result = sdk::outcome(
                {{"error", completion.error}}, descriptor.at("effects") == "read" ? "none" : "unknown", "failed");
        } else if (operation.at("kind") == "delivery") {
            const auto status = completion.result.at("status").get<std::string>();
            require(completion.result.at("id") == id && includes(Json::array({"sent", "not-sent", "unknown"}), status),
                "DELIVERY_RECEIPT_INVALID");
            if (status == "sent")
                require(!completion.result.at("receipt_id").get<std::string>().empty(), "DELIVERY_RECEIPT_MISSING");
            result = sdk::outcome(completion.result,
                status == "sent"       ? "confirmed"
                : status == "not-sent" ? "none"
                                       : "unknown",
                status == "sent" ? "succeeded" : "unknown");
        } else {
            result = completion.result;
            validate_outcome(result);
            if (operation.at("kind") == "action")
                sdk::validate_value(descriptor.at("output"), result);
        }
    } catch (const std::exception& error) {
        result = sdk::outcome(
            {{"error", failure(error)}}, descriptor.at("effects") == "read" ? "none" : "unknown", "failed");
    }

    for (const auto& child : store_.child_operations(id)) {
        if (child.at("state") != "settled")
            result["effects"] = "unknown";
        else if (result.at("effects") == "none" && child.at("result").at("effects") == "confirmed")
            result["effects"] = "confirmed";
    }

    operation["state"] = result.at("effects") == "unknown" ? "unknown" : "settled";
    operation["result"] = result;
    operation["revision"] = operation.at("revision").get<int>() + 1;
    writer_.save("operation", id, operation,
        operation.at("state") == "unknown" ? "operation.unknown" : "operation.settled", operation,
        operation.at("run_id"));
    if (done)
        done(operation, completion);
}

// 在权限和可用性满足时执行已登记动作。
// 步骤：校验授权并准备操作 → 绑定动作作用域 → 按能力查询就绪状态 → 复核任务与权限后派发。
std::string OperationCoordinator::action(
    const Json& run, const Json& descriptor, const Json& input, const std::string& key) {
    policy_.action(run, descriptor, input);

    auto operation = prepare(run, descriptor, input, key, "action");
    const auto id = operation.at("id").get<std::string>();

    auto scope = run_scope(run, "action", id);
    scope.root_action = descriptor.at("id");
    scope.registration_id = scopes_.action_registration(descriptor, run);

    if (operation.at("state") == "prepared") {
        const auto methods = plugins_.service(descriptor.at("service")).at("methods");
        if (methods.contains("readiness")) {
            auto readiness_scope = scope;
            readiness_scope.action_definition = read_descriptor(descriptor);
            calls_.request(descriptor.at("service"), "readiness", invocation(descriptor, input, id), readiness_scope,
                timeout_,
                // 在动作就绪查询完成后复核任务和权限，再派发操作。
                // 步骤：拒绝迟到或不可用结果 → 复核任务终态与意图 → 重新校验权限并派发或结算错误。
                [this, operation, scope](const Completion& readiness) {
                    if (readiness.late)
                        return;
                    if (!readiness.error.is_null() || !readiness.result.value("available", false)) {
                        Completion failed{{}, readiness.error.is_null()
                                                  ? sdk::error("ACTION_UNAVAILABLE", "unavailable")
                                                  : readiness.error};
                        auto value = store_.get("operation", operation.at("id"));
                        value["state"] = "settled";
                        value["result"] = sdk::outcome({{"error", failed.error}}, "none", "failed");
                        writer_.save(
                            "operation", value.at("id"), value, "operation.unavailable", failed.error, scope.run);
                        return;
                    }

                    auto current_run = store_.get("run", scope.run);
                    if (terminal(current_run) || current_run.at("intent_revision") != scope.intent)
                        return;
                    try {
                        policy_.action(current_run, operation.at("descriptor"), operation.at("input"));
                        dispatch(operation, scope);
                    } catch (const std::exception& error) {
                        settle(operation.at("id"), {{}, failure(error)});
                    }
                });
        } else
            dispatch(operation, scope);
    }
    return id;
}

// 校验回复授权并以稳定标识准备、派发渠道交付操作。
// 步骤：校验交付策略与渠道授权 → 构造交付描述和标识 → 准备操作并派发。
std::string OperationCoordinator::deliver(const Json& run, const Json& plan) {
    policy_.delivery(run, plan);
    conversations_.authorize(plan.at("binding"));

    const auto service = plan.at("binding").at("channel").at("service").get<std::string>();
    auto descriptor = plugins_.method(service, "deliver");
    descriptor["service"] = service;
    descriptor["method"] = "deliver";
    descriptor["query"] = "query_delivery";
    auto input = plan;
    input["id"] = run.at("id").get<std::string>() + ":delivery:" + std::to_string(run.at("intent_revision").get<int>());

    auto operation =
        prepare(run, descriptor, input, "delivery:" + std::to_string(run.at("intent_revision").get<int>()), "delivery");
    dispatch(operation, run_scope(run, "delivery", operation.at("id")));
    return operation.at("id");
}

// 协调插件发起的嵌套服务调用，并为写入建立子操作。
// 步骤：验证父调用、依赖与受控入口 → 检查委托权限和期限 → 构造回复处理 → 分别派发只读调用或持久化写操作。
void OperationCoordinator::nested(const PluginMessage& message) {
    const auto parent = calls_.owned(message.plugin, message.body.at("call_id"));
    const auto name = message.body.at("binding").get<std::string>();
    require(plugins_.dependency(message.plugin, name), "DEPENDENCY_ACCESS_DENIED");
    require(parent.scope.bindings.contains(name), "RUN_BINDING_DENIED");
    const auto service = parent.scope.bindings.at(name).get<std::string>();
    const auto method = message.body.at("method").get<std::string>();

    auto descriptor = plugins_.method(service, method);
    require(plugins_.service(service).at("contract") != "dafeiyu.model", "MODEL_SCHEDULING_REQUIRED");
    require(plugins_.service(service).at("contract") != "dafeiyu.action-executor", "ACTION_DISPATCH_REQUIRED");
    if (plugins_.service(service).at("contract") == "dafeiyu.channel") {
        require(method != "deliver", "CHANNEL_DELIVERY_COORDINATOR_REQUIRED");
        if (method == "query_delivery") {
            const auto recorded = store_.get("operation", message.body.at("input").at("id"));
            require(!recorded.is_null() && recorded.at("run_id") == parent.scope.run &&
                        recorded.at("kind") == "delivery" && recorded.at("descriptor").at("service") == service,
                "DELIVERY_QUERY_SCOPE_DENIED");
            conversations_.validate_identity(recorded.at("input").at("binding"));
        }
    }

    policy_.method(parent.scope, descriptor);
    const auto reply =
        // 将子调用结果交回仍有效的父调用，失效时仅允许必要的终止通知。
        // 步骤：忽略迟到结果 → 检查父调用资格 → 发送终止通知或正常业务回复。
        [this, source = message.plugin, key = message.body.at("key"), ticket = parent.id](const Completion& value) {
            if (value.late)
                return;
            if (!calls_.active(ticket)) {
                // 异步调用方可以结束清理，但失效后不能再收到可用于业务推进的结果。
                if (calls_.exists(ticket) && (!value.physical || !value.error.is_null()))
                    plugins_.send(
                        source, {{"type", "reply"}, {"key", key}, {"call_id", ticket}, {"result", nullptr},
                                    {"error", sdk::error("PARENT_CALL_CLOSED", "cancelled")}, {"terminal_only", true}});
                return;
            }
            plugins_.send(source, {{"type", "reply"}, {"key", key}, {"call_id", ticket}, {"result", value.result},
                                      {"error", value.error}});
        };

    auto child_scope = parent.scope;
    child_scope.parent_call = parent.id;
    child_scope.role = "service";
    child_scope.action_definition = nullptr;
    const auto requested_timeout = message.body.value("timeout_ms", timeout_);
    require(requested_timeout > 0, "CALL_TIMEOUT_INVALID");
    const auto child_timeout = std::min(timeout_, requested_timeout);
    child_scope.timeout_ms = child_timeout;

    if (descriptor.at("effects") == "read") {
        calls_.request(service, method, message.body.at("input"), child_scope, child_timeout, reply);
    } else {
        descriptor["service"] = service;
        descriptor["method"] = method;
        auto run = store_.get("run", parent.scope.run);
        auto operation = prepare(
            run, descriptor, message.body.at("input"), message.body.at("key"), "service", parent.scope.operation);
        dispatch(operation, child_scope, [reply](const Json& value, const Completion& completion) {
            if (value.at("state") == "settled" || value.at("state") == "unknown")
                reply({value.at("result"), {}, completion.late, completion.physical});
            else
                reply({{}, sdk::error("SERVICE_NOT_DISPATCHED", "unavailable"), completion.late, completion.physical});
        });
    }
}

// 通过原服务查询操作结果，为未决副作用补充证据。
// 步骤：读取操作及原绑定 → 校验渠道或动作身份 → 组装只读查询 → 成功回调中结算结果。
void OperationCoordinator::query(const std::string& id) {
    auto operation = store_.get("operation", id);
    require(!operation.is_null(), "OPERATION_MISSING");
    const auto& descriptor = operation.at("descriptor");
    const auto query_method = descriptor.value("query", std::string("query_operation"));

    auto run = store_.get("run", operation.at("run_id"));
    require(plugins_.matches(run.at("bindings")), "BINDING_CHANGED");
    if (operation.at("kind") == "delivery")
        conversations_.validate_identity(operation.at("input").at("binding"));

    auto scope = run_scope(run, "reconcile", id);
    auto query_input = Json{{"id", id}, {"input", operation.at("input")}};
    if (operation.at("kind") == "action") {
        scope.action_definition = read_descriptor(descriptor);
        query_input = invocation(descriptor, operation.at("input"), id);
    }

    calls_.request(
        descriptor.at("service"), query_method, query_input, scope, timeout_, [this, id](const Completion& result) {
            if (result.error.is_null())
                settle(id, result);
        });
}

// 恢复中断操作，对无法确认的写入保留未知副作用状态。
// 步骤：筛选未交接作业的中断操作 → 将读取重置为准备或写入标为未知 → 尝试查询写入结果。
void OperationCoordinator::recover() {
    for (auto operation : store_.recoverable_operations()) {
        if (operation.at("state") != "running" && operation.at("state") != "job")
            continue;
        if (!operation.at("job_id").is_null())
            continue;

        operation["state"] = operation.at("descriptor").at("effects") == "read" ? "prepared" : "unknown";
        writer_.save(
            "operation", operation.at("id"), operation, "operation.interrupted", operation, operation.at("run_id"));
        if (operation.at("state") == "unknown") {
            try {
                query(operation.at("id"));
            } catch (const std::exception&) { // 原绑定可能已不可用，保留未决状态。
            }
        }
    }
}

// 依据带版本和证据的核对命令结算未知副作用。
// 步骤：校验操作状态与版本 → 验证理由、证据及明确结果 → 事务记录核对事实并结算。
Json OperationCoordinator::reconcile(const Json& command) {
    auto value = store_.get("operation", command.at("id"));
    require(!value.is_null() && value.at("state") == "unknown", "OPERATION_NOT_UNRESOLVED");
    require(value.at("revision") == command.at("expected_revision"), "OPERATION_REVISION_CONFLICT");

    require(!command.at("reason").get<std::string>().empty() && !command.at("evidence").get<std::string>().empty(),
        "RECONCILIATION_EVIDENCE_REQUIRED");
    validate_outcome(command.at("result"));
    require(command.at("result").at("effects") != "unknown", "RECONCILIATION_UNRESOLVED");

    store_.atomic([&] {
        writer_.event("operation.reconciliation", command, value.at("run_id"));
        settle(value.at("id"), {command.at("result"), {}});
    });
    return store_.get("operation", value.at("id"));
}

// 将已注册后台作业绑定到仍在运行的所属操作。
void OperationCoordinator::attach_job(const std::string& operation_id, const std::string& job_id) {
    auto value = store_.get("operation", operation_id);
    require(!value.is_null() && value.at("state") == "running", "JOB_OPERATION_NOT_RUNNING");
    require(value.at("job_id").is_null() || value.at("job_id") == job_id, "OPERATION_ALREADY_HAS_JOB");
    value["job_id"] = job_id;
    writer_.save(
        "operation", operation_id, value, "operation.job_registered", {{"job_id", job_id}}, value.at("run_id"));
}

}
