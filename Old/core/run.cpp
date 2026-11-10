#include "run.hpp"
#include "run_view.hpp"

namespace dfy {
// 连接任务执行依赖，并领取运行任务、策略决策和模型尝试的写权限。
RunExecutor::RunExecutor(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy,
    OperationCoordinator& operations, JobCoordinator& jobs, ContextAssembler& context, const Profile& profile)
    : store_(store), writer_(store.claim("runs", {"run", "decision", "attempt"})), plugins_(plugins), calls_(calls),
      policy_(policy), operations_(operations), jobs_(jobs), context_(context), profile_(profile),
      timeout_(profile.data.at("limits").at("call_timeout_ms")) {
}

// 递增运行任务版本，并原子保存状态及对应事实。
void RunExecutor::save(Json run, const std::string& event, const Json& payload) {
    run["revision"] = run.at("revision").get<int>() + 1;
    const auto id = run.at("id").get<std::string>();
    writer_.save("run", id, run, event, payload, id);
}

// 将未终止任务标记为失败或等待恢复，并记录错误。
void RunExecutor::fail(const std::string& id, const Json& error, bool wait) {
    auto run = store_.get("run", id);
    if (terminal(run))
        return;

    run["execution"] = wait ? "waiting" : "failed";
    run["error"] = error;
    run["wait_reason"] = wait ? Json{{"type", "interrupted"}} : Json{};
    save(run, wait ? "run.waiting" : "run.failed", error);
}

// 判断异步结果是否仍对应未终止任务的当前意图版本。
bool RunExecutor::current(const std::string& id, int intent) const {
    auto run = store_.get("run", id);
    return !terminal(run) && run.at("intent_revision") == intent;
}

// 将运行任务状态和调用预算转换为公开决策视图。
Json RunExecutor::view(const std::string& id) {
    const auto run = store_.get("run", id);
    return run.is_null() ? Json{} : make_run_view(run, store_.get("call_budget", "run:" + id)).to_json();
}

// 分页读取运行任务，并逐项转换为公开视图。
Json RunExecutor::views(uint64_t cursor, size_t limit) {
    auto page = store_.page("run", cursor, limit);
    for (auto& run : page["items"])
        run = view(run.at("id"));
    return page;
}

// 将收到新结算证据的等待任务重新加入可调度队列。
// 步骤：读取已结算或未知操作 → 找出关联等待任务 → 按操作版本去重并更新任务。
void RunExecutor::operation_changed(const std::string& id) {
    const auto operation = store_.get("operation", id);
    if (operation.is_null() || (operation.at("state") != "settled" && operation.at("state") != "unknown"))
        return;

    for (auto run : store_.runs_waiting_for_operation(id)) {
        const auto revision = operation.at("revision").get<int>();
        if (run.value("observed_operation_revision", 0) >= revision)
            continue;

        run["observed_operation_revision"] = revision;
        run["execution"] = "queued";
        save(run, "run.operation_ready", {{"operation_id", id}, {"revision", revision}});
    }
}

// 为已终止的活动任务幂等记录结算反馈完成状态。
void RunExecutor::feedback_completed(const std::string& id, const Json& result) {
    auto run = store_.get("run", id);
    require(!run.is_null() && terminal(run), "RUN_FEEDBACK_NOT_TERMINAL");
    if (run.value("activity_notified", false))
        return;
    run["activity_notified"] = true;
    save(run, "activity.settled", result);
}

// 取消指定会话中尚未终止的运行任务。
void RunExecutor::cancel_conversation(const std::string& id) {
    for (const auto& run : store_.runs_in_conversation(id))
        (void)cancel(run.at("id"));
}

// 采纳策略提案并持久化策略状态和决策记录。
// 步骤：校验提案类型与策略状态 → 设置执行阶段 → 事务保存决策和任务变化。
void RunExecutor::adopt(Json run, const Json& result, const std::string& decision) {
    const auto proposal = result.at("proposal");
    require(includes(Json::array({"request_model", "invoke_action", "wait", "finish"}), proposal.at("type")),
        "STRATEGY_PROPOSAL_INVALID");
    require(result.at("state").is_object(), "STRATEGY_STATE_INVALID");
    run["strategy_state"] = result.at("state");
    run["proposal"] = proposal;

    run["decision_id"] = decision;
    run["phase"] = "execute";
    run["execution"] = "queued";
    run["wait_reason"] = nullptr;

    store_.atomic([&] {
        writer_.put("decision", decision,
            {{"id", decision}, {"run_id", run.at("id")}, {"result", result},
                {"intent_revision", run.at("intent_revision")}});
        save(run, "strategy.adopted", {{"decision_id", decision}, {"result", result}});
    });
}

// 异步请求策略决策或收尾审核，并采纳仍适用的结果。
// 步骤：预留策略预算并构造视图 → 保存调用状态 → 提交策略调用 → 在回调中复核意图、采纳提案或允许交付。
void RunExecutor::strategy(Json run, bool review) {
    const auto id = run.at("id").get<std::string>();
    const int intent = run.at("intent_revision");
    require(run.at("strategy_steps").get<int>() < run.at("limits").at("strategy_steps").get<int>(),
        "STRATEGY_BUDGET_EXHAUSTED");
    run["strategy_steps"] = run.at("strategy_steps").get<int>() + 1;

    const auto decision = id + ":decision:" + std::to_string(run.at("strategy_steps").get<int>());
    run["phase"] = review ? "reviewing" : "deciding";
    run["execution"] = "running";
    Json request{{"decision_id", decision}, {"trigger", run.value("trigger", std::string("ready"))},
        {"run", make_run_view(run, store_.get("call_budget", "run:" + id)).to_json()},
        {"state", run.at("strategy_state")}};
    if (review)
        request["candidate"] = run.at("proposal");

    save(run, "strategy.request", request);
    busy_.insert(id);
    try {
        calls_.request(run.at("bindings").at("bindings").at("strategy"), review ? "review_finish" : "propose", request,
            run_scope(run, "strategy"), timeout_,
            // 消费策略决策或收尾审核结果，并只推进当前意图的运行任务。
            // 步骤：释放忙碌标记并排除迟到结果 → 处理调用错误 → 采纳提案或接受收尾 → 保存协议失败。
            [this, id, intent, decision, review](const Completion& result) {
                if (!result.late)
                    busy_.erase(id);
                if (result.late || !current(id, intent))
                    return;
                if (!result.error.is_null()) {
                    const auto category = result.error.value("category", std::string{});
                    fail(id, result.error, category == "deadline" || category == "cancelled");
                    return;
                }

                try {
                    auto value = store_.get("run", id);
                    if (review && result.result.value("accept", false)) {
                        value["strategy_state"] = result.result.at("state");
                        value["phase"] = "delivery";
                        value["execution"] = "queued";
                        store_.atomic([&] {
                            writer_.put(
                                "decision", decision, {{"id", decision}, {"run_id", id}, {"result", result.result}});
                            save(value, "strategy.finish_accepted", result.result);
                        });
                    } else {
                        if (review)
                            require(result.result.at("proposal").at("type") != "finish", "REVIEW_RECURSIVE_FINISH");
                        adopt(value, result.result, decision);
                    }
                } catch (const std::exception& error) {
                    fail(id, failure(error));
                }
            });
    } catch (...) {
        busy_.erase(id);
        throw;
    }
}

// 记录模型尝试失败，并按错误类型终止、等待或交回策略处理。
// 步骤：保存尝试错误 → 处理权限、预算及取消错误 → 清除候选并构造失败观察 → 重新进入决策阶段。
void RunExecutor::model_failed(
    const std::string& id, const std::string& attempt, const Json& error, const std::string& stage) {
    auto record = store_.get("attempt", attempt);
    record["state"] = "failed";
    record["error"] = error;
    writer_.save("attempt", attempt, record, "model.failure", record, id);

    const auto category = error.value("category", std::string{});
    if (category == "permission" || category == "budget") {
        fail(id, error);
        return;
    }
    // 显式取消任务已进入终态；实例关闭时则保留未完成工作以供恢复。
    if (category == "cancelled") {
        fail(id, error, true);
        return;
    }

    auto run = store_.get("run", id);
    if (terminal(run))
        return;
    run["candidate"] = nullptr;
    run["phase"] = "decision";
    run["execution"] = "queued";
    run["trigger"] = "model_failed";
    run["error"] = error;
    run["observation"] = {{"kind", "model_failed"}, {"stage", stage}, {"binding", record.at("binding")},
        {"attempt_id", attempt}, {"error", error}};
    save(run, "strategy.failure_routed", run.at("observation"));
}

// 组装上下文并异步调用选定模型，将有效输出转换为策略候选。
// 步骤：校验绑定并预留尝试预算 → 保存尝试并组装上下文 → 调用模型 → 在回调中校验候选、记录结果或路由失败。
void RunExecutor::model(Json run, const Json& proposal) {
    const auto id = run.at("id").get<std::string>();
    const int intent = run.at("intent_revision");

    require(run.at("model_attempts").get<int>() < run.at("limits").at("model_attempts").get<int>(),
        "MODEL_BUDGET_EXHAUSTED");
    const auto binding = proposal.value("binding", std::string("model"));
    require(
        (binding == "model" || binding.starts_with("model.")) && run.at("bindings").at("bindings").contains(binding),
        "MODEL_BINDING_DENIED");
    const auto service = run.at("bindings").at("bindings").at(binding).get<std::string>();

    run["model_attempts"] = run.at("model_attempts").get<int>() + 1;
    const auto attempt = id + ":model:" + std::to_string(run.at("model_attempts").get<int>());
    run["phase"] = "context";
    run["execution"] = "running";
    run["attempt_id"] = attempt;

    store_.atomic([&] {
        writer_.put("attempt", attempt,
            {{"id", attempt}, {"run_id", id}, {"binding", binding}, {"service", service}, {"state", "assembling"}});
        save(run, "context.started", {{"attempt_id", attempt}, {"binding", binding}});
    });

    busy_.insert(id);
    try {
        context_.assemble(run, proposal,
            // 接收组装后的上下文，固定人格并提交模型生成调用。
            // 步骤：复核任务意图及组装结果 → 保存人格、请求和尝试状态 → 提交模型调用 → 将派发异常路由为模型失败。
            [this, id, intent, attempt, service](const Completion& assembled) {
                if (!current(id, intent)) {
                    busy_.erase(id);
                    return;
                }

                if (!assembled.error.is_null()) {
                    busy_.erase(id);
                    model_failed(id, attempt, assembled.error, "context");
                    return;
                }
                try {
                    auto value = store_.get("run", id);
                    value["persona"] = assembled.result.at("persona");
                    require(value.at("persona").at("instance_id") == store_.instance_id(), "PERSONA_INSTANCE_MISMATCH");
                    value["phase"] = "model";
                    value["request"] = assembled.result;

                    auto record = store_.get("attempt", attempt);
                    record["request"] = assembled.result;
                    record["state"] = "running";
                    store_.atomic([&] {
                        writer_.put("attempt", attempt, record);
                        save(value, "model.request",
                            {{"attempt_id", attempt}, {"binding", record.at("binding")},
                                {"request", assembled.result}});
                    });

                    calls_.request(service, "generate", assembled.result, run_scope(value, "model"), timeout_,
                        // 记录模型调用完成证据，并将唯一有效候选交回策略。
                        // 步骤：保存尝试结果与迟到标记 → 复核当前意图并处理错误 → 校验模型事件 → 保存候选和观察或记录协议失败。
                        [this, id, intent, attempt](const Completion& output) {
                            if (!output.late)
                                busy_.erase(id);
                            auto record = store_.get("attempt", attempt);
                            record["state"] = output.error.is_null() ? "completed" : "failed";
                            record["result"] = output.result;
                            record["error"] = output.error;
                            record["late"] = output.late;
                            writer_.save("attempt", attempt, record, "model.settled", record, id);

                            if (output.late || !current(id, intent))
                                return;
                            if (!output.error.is_null()) {
                                model_failed(id, attempt, output.error, "model");
                                return;
                            }
                            try {
                                Json candidate;
                                for (const auto& event : output.result.at("events")) {
                                    const auto type = event.at("type").get<std::string>();
                                    if (type == "tool_call" || type == "final") {
                                        require(candidate.is_null(), "MODEL_MULTIPLE_DECISIONS");
                                        candidate = event;
                                    } else
                                        require(type == "text_delta" || type == "usage", "MODEL_EVENT_INVALID");
                                }
                                require(!candidate.is_null(), "MODEL_INCOMPLETE");

                                auto latest = store_.get("run", id);
                                latest["candidate"] = candidate;
                                latest["phase"] = "decision";
                                latest["execution"] = "queued";
                                latest["trigger"] = "model_settled";
                                latest["error"] = nullptr;
                                latest["observation"] = {{"kind", "model_succeeded"}, {"binding", record.at("binding")},
                                    {"attempt_id", attempt}};
                                save(latest, "model.candidate", candidate);
                            } catch (const std::exception& error) {
                                model_failed(id, attempt, failure(error), "protocol");
                            }
                        });
                } catch (const std::exception& error) {
                    busy_.erase(id);
                    model_failed(id, attempt, failure(error), "dispatch");
                }
            });
    } catch (const std::exception& error) {
        busy_.erase(id);
        model_failed(id, attempt, failure(error), "context");
    }
}

// 保存策略给出的目标结论与回答，并将运行任务置为完成。
void RunExecutor::complete(Json run) {
    run["execution"] = "completed";
    run["phase"] = "done";
    run["wait_reason"] = nullptr;

    run["goal"] = run.at("proposal").value("goal", std::string("uncertain"));
    require(includes(Json::array({"met", "partial", "unmet", "uncertain"}), run.at("goal")), "GOAL_INVALID");
    run["goal_origin"] = "strategy";

    run["answer"] = run.at("proposal").value("text", std::string{});
    save(run, "run.completed", {{"goal", run.at("goal")}, {"delivery", run.at("delivery")}});
}

// 根据终态、忙碌标记、等待操作或唤醒时间判断任务能否推进。
// 步骤：排除终止、忙碌或暂停任务 → 检查等待操作 → 检查定时等待条件。
bool RunExecutor::runnable(const Json& run) {
    if (terminal(run) || busy_.contains(run.at("id").get<std::string>()))
        return false;
    if (run.at("execution") == "paused")
        return false;

    if (run.at("execution") != "waiting")
        return true;
    if (!run.at("active_operation").is_null()) {
        const auto op = store_.get("operation", run.at("active_operation"));
        return !op.is_null() &&
               (op.at("state") == "settled" ||
                   (op.at("state") == "unknown" && run.at("wait_reason").value("type", std::string{}) != "effects"));
    }

    const auto& reason = run.at("wait_reason");
    return reason.is_object() && reason.value("type", std::string{}) == "time" &&
           now_ms() >= reason.at("until").get<int64_t>();
}

// 按持久化阶段推进运行任务，协调决策、动作、等待和交付。
// 步骤：验证绑定快照 → 按阶段派发策略或执行提案 → 消费操作结果或交付回执 → 保存等待、完成或错误状态。
void RunExecutor::advance(Json run) {
    const auto id = run.at("id").get<std::string>();
    try {
        require(plugins_.matches(run.at("bindings")), "BINDING_CHANGED");

        const auto phase = run.at("phase").get<std::string>();
        if (phase == "decision") {
            strategy(run, false);
            return;
        }
        if (phase == "review") {
            strategy(run, true);
            return;
        }
        if (phase == "wait") {
            run["phase"] = "decision";
            run["execution"] = "queued";
            run["trigger"] = "wait_resolved";
            save(run, "run.woken");
            return;
        }

        if (phase == "execute") {
            const auto proposal = run.at("proposal");
            const auto type = proposal.at("type").get<std::string>();
            if (type == "request_model") {
                model(run, proposal);
                return;
            }
            if (type == "invoke_action") {
                const auto action = proposal.at("action").get<std::string>();
                require(run.at("bindings").at("actions").contains(action), "ACTION_DIRECTORY_DENIED");
                if (run.at("candidate").is_object() &&
                    run.at("candidate").value("type", std::string{}) == "tool_call") {
                    bool visible = false;
                    for (const auto& item : run.at("request").at("actions"))
                        if (item.at("id") == action)
                            visible = true;
                    require(visible, "MODEL_ACTION_NOT_VISIBLE");
                }

                const auto operation = operations_.action(run, run.at("bindings").at("actions").at(action),
                    proposal.at("arguments"), "action:" + std::to_string(run.at("strategy_steps").get<int>()));
                run["observed_operation_revision"] = 0;
                run["active_operation"] = operation;
                run["phase"] = "action";
                run["execution"] = "waiting";
                run["wait_reason"] = {{"type", "operation"}, {"id", operation}};
                save(run, "run.operation_wait");
            } else if (type == "finish") {
                require(store_.run_operations(id, true).empty(), "RUN_EFFECT_UNRESOLVED");
                run["phase"] = "review";
                run["execution"] = "queued";
                save(run, "run.finish_candidate", proposal);
            } else {
                const auto reason = proposal.at("condition");
                require(reason.at("type") == "time" || reason.at("type") == "user", "WAIT_CONDITION_UNSUPPORTED");
                if (reason.at("type") == "time")
                    require(reason.at("until").get<int64_t>() > now_ms(), "WAIT_TIME_INVALID");
                run["phase"] = "wait";
                run["execution"] = "waiting";
                run["wait_reason"] = reason;
                save(run, "run.waiting", reason);
            }
        } else if (phase == "action") {
            auto operation = store_.get("operation", run.at("active_operation"));
            if (operation.at("state") == "unknown") {
                run["execution"] = "waiting";
                run["wait_reason"] = {{"type", "effects"}, {"operation_id", operation.at("id")}};
                save(run, "run.effects_wait");
                return;
            }

            if (operation.at("state") == "settled") {
                bool recorded = false;
                for (const auto& result : run.at("results"))
                    if (result.at("operation_id") == operation.at("id"))
                        recorded = true;

                if (!recorded)
                    run["results"].push_back(
                        {{"operation_id", operation.at("id")}, {"intent_revision", operation.at("intent_revision")},
                            {"action", run.at("proposal").at("action")}, {"outcome", operation.at("result")}});
                run["active_operation"] = nullptr;
                run["phase"] = "decision";
                run["execution"] = "queued";
                run["trigger"] = "operation_settled";
                run["candidate"] = nullptr;
                save(run, "run.operation_consumed", operation);
            }
        } else if (phase == "delivery") {
            if (run.at("delivery_policy") == "none") {
                complete(run);
                return;
            }
            if (run.at("active_operation").is_null()) {
                const Json plan{{"target", run.at("external_conversation")},
                    {"text", run.at("proposal").value("text", std::string{})},
                    {"intent_revision", run.at("intent_revision")}, {"binding", run.at("delivery_binding")}};
                run["observed_operation_revision"] = 0;
                run["active_operation"] = operations_.deliver(run, plan);
                run["execution"] = "waiting";
                run["delivery"] = "sending";
                run["wait_reason"] = {{"type", "delivery"}};
                save(run, "run.delivery_wait");
            } else {
                const auto operation = store_.get("operation", run.at("active_operation"));
                if (operation.at("state") == "unknown") {
                    run["delivery"] = "unknown";
                    run["execution"] = "waiting";
                    run["wait_reason"] = {{"type", "effects"}, {"operation_id", operation.at("id")}};
                    save(run, "run.delivery_unknown");
                    return;
                }
                if (operation.at("state") != "settled")
                    return;

                if (operation.at("intent_revision") != run.at("intent_revision")) {
                    run["active_operation"] = nullptr;
                    run["phase"] = "decision";
                    run["execution"] = "queued";
                    run["delivery"] = "none";
                    run["trigger"] = "input_revised";
                    run["candidate"] = nullptr;
                    save(run, "run.superseded_delivery", operation);
                    return;
                }

                const auto data = operation.at("result").at("data");
                if (data.value("status", std::string{}) == "sent") {
                    run["delivery"] = "sent";
                    complete(run);
                } else {
                    run["delivery"] = "not-sent";
                    run["execution"] = "paused";
                    save(run, "run.delivery_unconfirmed", data);
                }
            }
        }
    } catch (const std::exception& error) {
        const std::string reason = error.what();
        fail(id, failure(error),
            reason == "BINDING_CHANGED" || reason.starts_with("DELIVERY_AUTHORITY_") ||
                reason == "DELIVERY_BINDING_CHANGED" || reason.find("UNRESOLVED") != std::string::npos ||
                reason.find("NOT_READY") != std::string::npos);
    }
}

// 将中断运行任务恢复到可重新决策或派发准备操作的阶段。
// 步骤：遍历活动任务并核对绑定 → 重置中断的推理或审核阶段 → 恢复尚未派发的操作阶段。
void RunExecutor::recover() {
    for (auto run : store_.active_runs()) {
        if (terminal(run))
            continue;

        if (!plugins_.matches(run.at("bindings"))) {
            fail(run.at("id"), sdk::error("BINDING_CHANGED", "unavailable"), true);
            continue;
        }

        if (includes(Json::array({"context", "model", "deciding", "reviewing"}), run.at("phase"))) {
            run["phase"] = run.at("phase") == "reviewing" ? "review" : "decision";
            run["execution"] = "queued";
            run["trigger"] = "recovered";
            save(run, "run.recovered");
        } else if (!run.at("active_operation").is_null()) {
            auto operation = store_.get("operation", run.at("active_operation"));

            if (operation.at("state") == "prepared") {
                run["active_operation"] = nullptr;
                run["phase"] = run.at("phase") == "delivery" ? "delivery" : "execute";
                run["execution"] = "queued";
                save(run, "run.recovered");
            }
        }
    }
}

// 终止运行任务，并请求取消其服务调用和后台作业。
Json RunExecutor::cancel(const std::string& id) {
    auto run = store_.get("run", id);
    require(!run.is_null(), "RUN_MISSING");
    if (!terminal(run)) {
        run["execution"] = "cancelled";
        save(run, "run.cancelled");
        calls_.cancel_run(id);
        jobs_.cancel_jobs(id);
    }
    return store_.get("run", id);
}

// 用新输入修订运行意图，使旧调用结果失去业务推进资格。
// 步骤：校验任务可修订 → 取消旧调用 → 递增意图版本并更新输入 → 按在途操作情况保存后续阶段。
Json RunExecutor::steer(const std::string& id, const Json& input) {
    auto run = store_.get("run", id);
    require(!run.is_null() && !terminal(run), "RUN_NOT_STEERABLE");

    calls_.cancel_run(id);
    run = store_.get("run", id);

    run["intent_revision"] = run.at("intent_revision").get<int>() + 1;
    run["input"] = input;
    run["candidate"] = nullptr;
    run["trigger"] = "input_revised";
    run["observation"] = {{"kind", "input_revised"}};
    run["error"] = nullptr;
    if (run.at("active_operation").is_null()) {
        run["phase"] = "decision";
        run["execution"] = "queued";
    }

    save(run, "run.steered", input);
    return store_.get("run", id);
}

// 在绑定和在途调用条件允许时重新排队运行任务。
Json RunExecutor::resume(const std::string& id) {
    auto run = store_.get("run", id);
    require(!run.is_null() && !terminal(run), "RUN_NOT_RESUMABLE");

    require(plugins_.matches(run.at("bindings")), "BINDING_CHANGED");
    require(!calls_.pending_for(id), "CALL_STILL_RUNNING");

    if (run.at("active_operation").is_null())
        run["phase"] = "decision";
    run["execution"] = "queued";
    run["observation"] = {{"kind", "resumed"}};
    run["error"] = nullptr;
    save(run, "run.resumed");
    return store_.get("run", id);
}

// 从已接收输入创建携带授权和绑定快照的运行任务。
// 步骤：确定活动与交付身份 → 初始化阶段、预算和快照 → 绑定会话代次 → 事务保存任务及接纳事实。
std::string RunExecutor::create(const Json& input, const Json& grants) {
    const auto id = identifier();
    const auto& payload = input.at("input");

    const auto activity = payload.value("activity", std::string{});
    const auto& route = input.at("route");
    const std::string channel =
        route.at("delivery").is_null() ? "" : route.at("delivery").at("channel").at("service").get<std::string>();

    Json run{{"id", id}, {"input_id", input.at("id")}, {"input", payload}, {"actor", payload.at("actor")},
        {"conversation", route.at("id")}, {"conversation_key", route.at("key")},
        {"external_conversation", payload.at("conversation")}, {"delivery_binding", route.at("delivery")},
        {"activity", activity}, {"execution", "queued"}, {"phase", "decision"}, {"revision", 1}, {"intent_revision", 1},
        {"bindings", plugins_.snapshot(grants.at("actions"), channel)}, {"grants", grants},
        {"limits", profile_.data.at("limits")}, {"foundation", profile_.foundation}, {"persona", nullptr},
        {"strategy_state", Json::object()}, {"strategy_steps", 0}, {"model_attempts", 0}, {"results", Json::array()},
        {"candidate", nullptr}, {"proposal", nullptr}, {"request", nullptr}, {"active_operation", nullptr},
        {"wait_reason", nullptr}, {"delivery_policy", payload.value("delivery_policy", std::string("reply"))},
        {"delivery", "none"}, {"goal", "uncertain"}, {"goal_origin", "none"}, {"created_at", now_ms()},
        {"trigger", "run_ready"}, {"auto_depth", payload.value("auto_depth", 0)}, {"error", nullptr}};

    run["conversation_generation"] = input.at("conversation_generation");
    run["observed_operation_revision"] = 0;
    run["observation"] = {{"kind", "run_ready"}};

    store_.atomic([&] {
        writer_.put("run", id, run);
        writer_.event("run.accepted", run, id);
    });
    return id;
}

}
