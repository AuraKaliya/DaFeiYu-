#pragma once
#include "context.hpp"
#include "jobs.hpp"
namespace dfy {
class RunExecutor {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    PolicyGate& policy_;
    OperationCoordinator& operations_;
    JobCoordinator& jobs_;
    ContextAssembler& context_;
    const Profile& profile_;
    int timeout_;
    std::set<std::string> busy_;
    void save(Json run, const std::string& event, const Json& payload = Json::object());
    void fail(const std::string& id, const Json& error, bool wait = false);
    bool current(const std::string& id, int intent) const;
    void strategy(Json run, bool review);
    void model(Json run, const Json& proposal);
    void model_failed(const std::string& id, const std::string& attempt, const Json& error, const std::string& stage);
    void adopt(Json run, const Json& result, const std::string& decision);
    void complete(Json run);
public:
    RunExecutor(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy,
        OperationCoordinator& operations, JobCoordinator& jobs, ContextAssembler& context, const Profile& profile);
    bool busy() const { return !busy_.empty(); }
    bool runnable(const Json& run);
    void advance(Json run);
    void recover();
    Json cancel(const std::string& id);
    Json steer(const std::string& id, const Json& input);
    Json resume(const std::string& id);
    std::string create(const Json& input, const Json& grants);
    void operation_changed(const std::string& operation);
    void feedback_completed(const std::string& id, const Json& result);
    Json view(const std::string& id);
    Json views(uint64_t cursor, size_t limit);
    void cancel_conversation(const std::string& id);
};
}
