#pragma once
#include "policy.hpp"
#include "conversations.hpp"
#include "scopes.hpp"
namespace dfy {
class OperationCoordinator {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    PolicyGate& policy_;
    Conversations& conversations_;
    ScopeManager& scopes_;
    int timeout_;
    using Done = std::function<void(const Json&, const Completion&)>;
    Json prepare(const Json& run, const Json& descriptor, const Json& input, const std::string& key, const std::string& kind, const std::string& parent = {});
    void dispatch(Json operation, CallScope scope, Done done = {});
    void settle(const std::string& id, const Completion& completion, Done done = {});
    void blocked(const Json& descriptor, const std::string& except = {});
public:
    OperationCoordinator(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy, Conversations& conversations, ScopeManager& scopes, const Profile& profile);
    std::string action(const Json& run, const Json& descriptor, const Json& input, const std::string& key);
    std::string deliver(const Json& run, const Json& plan);
    void nested(const PluginMessage& message);
    void attach_job(const std::string& operation, const std::string& job);
    void settle_job(const std::string& operation, const Completion& result) { settle(operation, result); }
    void recover();
    Json reconcile(const Json& command);
    void query(const std::string& id);
};
CallScope run_scope(const Json& run, std::string role, std::string operation = {});
}
