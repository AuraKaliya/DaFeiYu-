#include "jobs.hpp"

namespace dfy {
// 连接后台作业依赖，并领取作业写权限及读取容量限制。
JobCoordinator::JobCoordinator(StateStore& store, PluginRuntime& plugins, CallRouter& calls,
    OperationCoordinator& operations, const Profile& profile)
    : store_(store), writer_(store.claim("jobs", {"job"})), plugins_(plugins), calls_(calls), operations_(operations),
      timeout_(profile.data.at("limits").at("call_timeout_ms")), max_jobs_(profile.data.at("limits").at("max_jobs")) {
}

// 为动作调用登记可恢复的后台作业，并返回持久化注册回执。
// 步骤：校验调用归属与作业能力 → 复用或创建作业 → 事务关联操作 → 开放结果回传并发送回执。
void JobCoordinator::register_job(const PluginMessage& message) {
    const auto parent = calls_.owned(message.plugin, message.body.at("call_id"));
    require(!parent.scope.operation.empty() && parent.scope.role == "action", "JOB_SCOPE_DENIED");
    const auto methods = plugins_.service(parent.service).at("methods");
    for (const auto* method : {"query_job", "reattach_job", "cancel_job"})
        require(methods.contains(method), "JOB_CAPABILITY_REQUIRED");

    auto operation = store_.get("operation", parent.scope.operation);
    require(operation.at("descriptor").at("service") == parent.service && operation.at("state") == "running",
        "JOB_OPERATION_OWNER_DENIED");
    const auto work = message.body.at("work_key").get<std::string>();
    require(!work.empty() && work.size() <= 128, "JOB_WORK_KEY_INVALID");

    const auto id = parent.scope.operation + ":job:" + fingerprint(work);
    auto job = store_.get("job", id);
    if (!job.is_null())
        require(job.at("work_key") == work && job.at("plugin") == message.plugin, "JOB_REGISTRATION_CONFLICT");

    if (job.is_null()) {
        const auto count = store_.active_jobs().size();
        require(count < static_cast<size_t>(max_jobs_), "JOB_LIMIT");
        require(operation.at("job_id").is_null(), "OPERATION_ALREADY_HAS_JOB");
        job = {{"id", id}, {"run_id", parent.scope.run}, {"operation_id", parent.scope.operation},
            {"plugin", message.plugin}, {"service", parent.service}, {"work_key", work}, {"state", "registered"},
            {"sequence", 0}, {"result", nullptr}, {"cancel_requested", false}};
        operation["job_id"] = id;
        store_.atomic([&] {
            writer_.put("job", id, job);
            operations_.attach_job(parent.scope.operation, id);
            writer_.event("job.registered", job, parent.scope.run);
        });
    }

    plugins_.allow_job(message.plugin, id);
    plugins_.send(message.plugin, {{"type", "reply"}, {"call_id", parent.id}, {"key", message.body.at("key")},
                                      {"result", job}, {"error", nullptr}});
}

// 幂等接收后台作业的终态结果，并同步结算所属操作。
// 步骤：核对归属与结果序号 → 校验或复用终态 → 事务保存作业、操作及事实 → 回传接收回执。
void JobCoordinator::job_result(const PluginMessage& message) {
    const auto id = message.body.at("job_id").get<std::string>();
    auto job = store_.get("job", id);
    require(!job.is_null() && job.at("plugin") == message.plugin, "JOB_OWNER_DENIED");
    const auto sequence = message.body.at("sequence").get<uint64_t>();
    require(sequence > 0, "JOB_SEQUENCE_INVALID");
    if (job.at("state") == "completed")
        require(
            job.at("sequence") == sequence && job.at("result") == message.body.at("result"), "JOB_TERMINAL_CONFLICT");
    else {
        validate_outcome(message.body.at("result"));
        job["state"] = "completed";
        job["sequence"] = sequence;
        job["result"] = message.body.at("result");
        store_.atomic([&] {
            writer_.put("job", id, job);
            operations_.settle_job(job.at("operation_id"), {job.at("result"), {}});
            writer_.event("job.completed", job, job.at("run_id"));
        });
    }

    plugins_.send(message.plugin, {{"type", "job_receipt"}, {"job_id", id}, {"sequence", sequence}});
}

// 记录运行任务的作业取消请求，并尝试通知执行插件。
// 步骤：筛选尚未请求取消的作业 → 持久化取消意图 → 提交取消调用或记录不可用原因。
void JobCoordinator::cancel_jobs(const std::string& run_id) {
    for (auto job : store_.active_jobs(run_id)) {
        if (job.at("run_id") != run_id || job.at("state") == "completed" || job.at("cancel_requested") == true)
            continue;
        job["cancel_requested"] = true;
        writer_.save("job", job.at("id"), job, "job.cancel_requested", job, run_id);

        auto run = store_.get("run", run_id);
        try {
            calls_.request(job.at("service"), "cancel_job", job, run_scope(run, "job_control", job.at("operation_id")),
                timeout_, [](const Completion&) {});
        } catch (const std::exception& error) {
            store_.atomic([&] { writer_.event("job.cancel_unavailable", failure(error), run_id); });
        }
    }
}

// 恢复未完成后台作业的结果查询与观察连接。
// 步骤：校验原绑定并恢复回传资格 → 查询作业 → 按完成、运行或中断状态接收结果、重新连接或保留核对信息。
void JobCoordinator::recover() {
    for (auto job : store_.active_jobs()) {
        if (job.at("state") == "completed")
            continue;
        auto run = store_.get("run", job.at("run_id"));
        try {
            require(plugins_.matches(run.at("bindings")), "BINDING_CHANGED");

            plugins_.allow_job(job.at("plugin"), job.at("id"));
            calls_.request(job.at("service"), "query_job", job, run_scope(run, "job_recovery", job.at("operation_id")),
                timeout_,
                // 依据后台作业查询结果恢复结算或结果观察。
                // 步骤：忽略迟到回复 → 接收已完成结果或重新连接运行作业 → 无法确认时标记中断结果。
                [this, job, run](const Completion& result) {
                    if (result.late)
                        return;

                    if (result.error.is_null() && result.result.value("state", std::string{}) == "completed") {
                        job_result({job.at("plugin"),
                            {{"job_id", job.at("id")}, {"sequence", 1}, {"result", result.result.at("result")}}});
                    } else if (result.error.is_null() && result.result.value("state", std::string{}) == "running") {
                        calls_.request(job.at("service"), "reattach_job", job,
                            run_scope(run, "job_recovery", job.at("operation_id")), timeout_, [](const Completion&) {});
                    } else {
                        operations_.settle_job(
                            job.at("operation_id"), {{}, sdk::error("JOB_INTERRUPTED", "unavailable", "unknown")});
                    }
                });
        } catch (const std::exception&) { // 保留已登记工作，等待显式核对结果。
        }
    }
}
}
