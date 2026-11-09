#pragma once
#include "context.hpp"
namespace dfy {
class RunExecutor {
    StateStore& store_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    PolicyGate& policy_;
    OperationCoordinator& operations_;
    ContextAssembler& context_;
    int timeout_;
    std::set<std::string> busy_;
    void save(Json run, const std::string& event, const Json& payload = Json::object());
    void fail(const std::string& id, const Json& error, bool wait = false);
    bool current(const std::string& id, int intent) const;
    void strategy(Json run, bool review);
    void model(Json run, const Json& proposal);
    void adopt(Json run, const Json& result, const std::string& decision);
    void complete(Json run);
public:
    RunExecutor(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy,
        OperationCoordinator& operations, ContextAssembler& context, const Profile& profile);
    bool busy() const { return !busy_.empty(); }
    bool runnable(const Json& run);
    void advance(Json run);
    void recover();
    Json cancel(const std::string& id);
    Json steer(const std::string& id, const Json& input);
    Json resume(const std::string& id);
};
}
