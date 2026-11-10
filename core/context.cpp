#include "context.hpp"
#include "capabilities.hpp"

namespace dfy {
// 连接上下文采集依赖，并读取调用超时和上下文字节预算。
ContextAssembler::ContextAssembler(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy,
    ScopeManager& scopes, const Profile& profile)
    : store_(store), plugins_(plugins), calls_(calls), policy_(policy), scopes_(scopes),
      timeout_(profile.data.at("limits").at("call_timeout_ms")),
      budget_(profile.data.at("limits").value("context_bytes", size_t{262144})) {
}

// 异步汇总运行任务所需的记忆、人格、上下文及可见动作。
// 步骤：建立请求和采集序列 → 依次调用记忆及上下文服务 → 根据当前权限筛选动作 → 检查总大小并交付结果。
void ContextAssembler::assemble(const Json& run, const Json& proposal, std::function<void(const Completion&)> done) {
    struct Assembly {
        Json request, run, requested_actions;
        std::vector<std::tuple<std::string, std::string, std::string>> steps;
        size_t next{};
        bool finished{};
        std::function<void(const Completion&)> done;
    };

    auto work = std::make_shared<Assembly>();
    work->run = run;
    work->requested_actions = proposal.value("actions", run.at("grants").at("actions"));
    work->done = std::move(done);
    work->request = {{"foundation", run.at("foundation")}, {"persona", run.at("persona")}, {"input", run.at("input")},
        {"results", run.at("results")}, {"memory", Json::array()}, {"context", Json::array()},
        {"actions", Json::array()}, {"behavior", store_.get("system", "behavior")},
        {"purpose", proposal.value("purpose", std::string("respond"))}, {"strategy_state", run.at("strategy_state")},
        {"intent_revision", run.at("intent_revision")}};

    const auto memory = run.at("bindings").at("bindings").at("memory").get<std::string>();
    work->steps.emplace_back(memory, run.at("persona").is_null() ? "snapshot" : "validate", "");
    work->steps.emplace_back(memory, "retrieve", "");
    for (const auto& [name, service] : run.at("bindings").at("bindings").items())
        if (name.starts_with("context."))
            work->steps.emplace_back(service.get<std::string>(), "collect", "");
    for (const auto& [service, token] : scopes_.contexts(run))
        work->steps.emplace_back(service, "collect", token);

    // 续体以弱引用指向自身，避免回调相互持有造成生命周期循环。
    auto next = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = next;
    *next =
        // 推进上下文采集续体，完成后筛选动作并检查总字节预算。
        // 步骤：判断采集是否结束 → 汇总可见动作或提交下一步读取 → 通过回调继续推进或交付错误。
        [this, work, weak] {
            if (work->finished)
                return;

            if (work->next == work->steps.size()) {
                const auto current = policy_.current();
                for (const auto& name : work->requested_actions) {
                    require(work->run.at("bindings").at("actions").contains(name.get<std::string>()),
                        "ACTION_DIRECTORY_DENIED");
                    const auto& action = work->run.at("bindings").at("actions").at(name.get<std::string>());
                    if (!includes(current.at("actions"), name) ||
                        !includes(current.at("effects"), action.at("effects")) || !plugins_.ready(action.at("service")))
                        continue;
                    bool allowed = true;
                    for (const auto& resource : resources_of(action))
                        if (!permits(current.at("resources"), resource) ||
                            !permits(work->run.at("grants").at("resources"), resource))
                            allowed = false;
                    if (!allowed)
                        continue;
                    try {
                        (void)scopes_.action_registration(action, work->run);
                    } catch (const std::exception&) {
                        continue;
                    }
                    work->request["actions"].push_back(action);
                }

                work->finished = true;
                if (work->request.dump().size() > budget_)
                    work->done({{}, sdk::error("CONTEXT_BUDGET", "budget")});
                else
                    work->done({work->request, {}});
                return;
            }

            const auto [service, method, registration] = work->steps.at(work->next++);
            Json input{{"text", work->run.at("input").at("text")}, {"scope", work->run.at("conversation")},
                {"budget", budget_}};
            if (method == "validate")
                input = work->run.at("persona");
            auto continuation = weak.lock();
            if (!registration.empty() && !scopes_.registration_active(registration)) {
                (*continuation)();
                return;
            }

            try {
                auto scope = run_scope(work->run, "context");
                scope.registration_id = registration;
                calls_.request(service, method, input, scope, timeout_,
                    // 消费单次记忆或上下文采集结果，并推进后续采集。
                    // 步骤：忽略过期或已结束结果 → 校验并合并对应类型数据 → 执行下一续体或报告失败。
                    [work, method, continuation](const Completion& result) {
                        if (work->finished || result.late)
                            return;
                        if (!result.error.is_null()) {
                            work->finished = true;
                            work->done(result);
                            return;
                        }

                        try {
                            if (method == "snapshot") {
                                require(result.result.at("instance_id").is_string() &&
                                            result.result.at("revision").is_number_integer() &&
                                            result.result.at("persona").is_string(),
                                    "PERSONA_INVALID");
                                work->request["persona"] = result.result;
                            } else if (method == "validate")
                                require(result.result.at("valid") == true, "PERSONA_INVALID");
                            else if (method == "retrieve") {
                                require(result.result.at("items").is_array(), "MEMORY_ITEMS_INVALID");
                                work->request["memory"] = result.result.at("items");
                            } else {
                                for (const auto& item : result.result.at("items")) {
                                    require(item.at("source").is_string() && item.at("revision").is_number_integer() &&
                                                item.at("content").is_string(),
                                        "CONTEXT_ITEM_INVALID");
                                    require(item.at("scope") == "instance" ||
                                                item.at("scope") == work->run.at("conversation"),
                                        "CONTEXT_SCOPE_DENIED");
                                    if (item.value("expires_at", int64_t{0}) &&
                                        item.at("expires_at").get<int64_t>() < now_ms())
                                        continue;
                                    work->request["context"].push_back(item);
                                }
                            }

                            (*continuation)();
                        } catch (const std::exception& error) {
                            work->finished = true;
                            work->done({{}, failure(error)});
                        }
                    });
            } catch (const std::exception& error) {
                work->finished = true;
                work->done({{}, failure(error)});
            }
        };
    (*next)();
}
}
