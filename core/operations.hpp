#pragma once
#include "policy.hpp"
namespace dfy {
class OperationCoordinator {
    StateStore& store_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    PolicyGate& policy_;
    int timeout_, max_jobs_;
    using Done = std::function<void(const Json&)>;
    Json prepare(const Json& run, const Json& descriptor, const Json& input, const std::string& key, const std::string& kind, const std::string& parent = {});
    void dispatch(Json operation, CallScope scope, Done done = {});
    void settle(const std::string& id, const Completion& completion, Done done = {});
    void blocked(const Json& descriptor, const std::string& except = {});
public:
    OperationCoordinator(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy, const Profile& profile);
    std::string action(const Json& run, const Json& descriptor, const Json& input, const std::string& key);
    std::string deliver(const Json& run, const Json& plan);
    void nested(const PluginMessage& message);
    void register_job(const PluginMessage& message);
    void job_result(const PluginMessage& message);
    void cancel_jobs(const std::string& run);
    void recover();
    Json reconcile(const Json& command);
    void query(const std::string& id);
};
CallScope run_scope(const Json& run, std::string role, std::string operation = {});
}
