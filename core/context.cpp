#include "context.hpp"
namespace dfy {
ContextAssembler::ContextAssembler(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy, const Profile& profile)
    : store_(store), plugins_(plugins), calls_(calls), policy_(policy), timeout_(profile.data.at("limits").at("call_timeout_ms")),
      budget_(profile.data.at("limits").value("context_bytes", size_t{262144})) {}
void ContextAssembler::assemble(const Json& run, const Json& proposal, std::function<void(const Completion&)> done) {
    struct Assembly {
        Json request, run;
        std::vector<std::pair<std::string, std::string>> steps;
        size_t next{};
        bool finished{};
        std::function<void(const Completion&)> done;
    };
    auto work = std::make_shared<Assembly>();
    work->run = run; work->done = std::move(done);
    work->request = {{"foundation", run.at("foundation")}, {"persona", run.at("persona")}, {"input", run.at("input")},
        {"results", run.at("results")}, {"memory", Json::array()}, {"context", Json::array()}, {"actions", Json::array()},
        {"behavior", store_.get("system", "behavior")}, {"purpose", proposal.value("purpose", std::string("respond"))},
        {"strategy_state", run.at("strategy_state")}, {"intent_revision", run.at("intent_revision")}};
    const auto current = policy_.current();
    for (const auto& name : proposal.value("actions", run.at("grants").at("actions"))) {
        require(run.at("bindings").at("actions").contains(name.get<std::string>()), "ACTION_DIRECTORY_DENIED");
        const auto& action = run.at("bindings").at("actions").at(name.get<std::string>());
        if (!includes(current.at("actions"), name) || !includes(current.at("effects"), action.at("effects")) || !plugins_.ready(action.at("service"))) continue;
        if ((!includes(current.at("resources"), "*") && !includes(current.at("resources"), action.at("resource"))) ||
            (!includes(run.at("grants").at("resources"), "*") && !includes(run.at("grants").at("resources"), action.at("resource")))) continue;
        work->request["actions"].push_back(action);
    }
    const auto memory = run.at("bindings").at("bindings").at("memory").get<std::string>();
    work->steps.emplace_back(memory, run.at("persona").is_null() ? "snapshot" : "validate");
    work->steps.emplace_back(memory, "retrieve");
    for (const auto& [name, service] : run.at("bindings").at("bindings").items())
        if (name.starts_with("context.")) work->steps.emplace_back(service.get<std::string>(), "collect");
    // Continuations hold a weak reference to themselves; no persistent callback cycle.
    auto next = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = next;
    *next = [this, work, weak] {
        if (work->finished) return;
        if (work->next == work->steps.size()) {
            work->finished = true;
            if (work->request.dump().size() > budget_) work->done({{}, sdk::error("CONTEXT_BUDGET", "budget")});
            else work->done({work->request, {}});
            return;
        }
        const auto [service, method] = work->steps.at(work->next++);
        Json input{{"text", work->run.at("input").at("text")}, {"scope", work->run.at("conversation")}, {"budget", budget_}};
        if (method == "validate") input = work->run.at("persona");
        auto continuation = weak.lock();
        try {
            calls_.request(service, method, input, run_scope(work->run, "context"), timeout_,
                [work, method, continuation](const Completion& result) {
                    if (work->finished || result.late) return;
                    if (!result.error.is_null()) { work->finished = true; work->done(result); return; }
                    try {
                        if (method == "snapshot") {
                            require(result.result.at("instance_id").is_string() && result.result.at("revision").is_number_integer() && result.result.at("persona").is_string(), "PERSONA_INVALID");
                            work->request["persona"] = result.result;
                        } else if (method == "validate") require(result.result.at("valid") == true, "PERSONA_INVALID");
                        else if (method == "retrieve") { require(result.result.at("items").is_array(), "MEMORY_ITEMS_INVALID"); work->request["memory"] = result.result.at("items"); }
                        else {
                            for (const auto& item : result.result.at("items")) {
                                require(item.at("source").is_string() && item.at("revision").is_number_integer() && item.at("content").is_string(), "CONTEXT_ITEM_INVALID");
                                require(item.at("scope") == "instance" || item.at("scope") == work->run.at("conversation"), "CONTEXT_SCOPE_DENIED");
                                if (item.value("expires_at", int64_t{0}) && item.at("expires_at").get<int64_t>() < now_ms()) continue;
                                work->request["context"].push_back(item);
                            }
                        }
                        (*continuation)();
                    } catch (const std::exception& error) { work->finished = true; work->done({{}, failure(error)}); }
                });
        } catch (const std::exception& error) { work->finished = true; work->done({{}, failure(error)}); }
    };
    (*next)();
}
}
