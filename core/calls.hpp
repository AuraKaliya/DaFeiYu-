#pragma once
#include "plugins.hpp"
#include "store.hpp"

namespace dfy {
struct CallScope {
    std::string run, operation, role, root_action;
    int intent{};
    Json bindings = Json::object();
    Json grants = Json::object();
    std::string effects{"read"}, resource;
    Json delegation = Json::object();
    uint64_t parent_call{};
    std::string budget_id;
    int nested_limit{64};
    int timeout_ms{};
    Json context() const;
};
struct Completion { Json result; Json error; bool late{}; bool physical{true}; };
struct PendingCall {
    uint64_t id;
    std::string plugin, service, method;
    CallScope scope;
    SteadyClock::time_point deadline;
    bool logical_done{};
    std::function<void(const Completion&)> done;
};
class CallRouter {
    PluginRuntime& plugins_;
    StateStore& store_;
    uint64_t next_{1};
    std::map<uint64_t, PendingCall> pending_;
    const std::string epoch_ = identifier();
    void terminate(uint64_t root, const std::string& code);
public:
    CallRouter(PluginRuntime& plugins, StateStore& store) : plugins_(plugins), store_(store) {}
    uint64_t request(const std::string& service, const std::string& method, const Json& input, CallScope scope,
        int timeout_ms, std::function<void(const Completion&)> done);
    const PendingCall& owned(const std::string& plugin, uint64_t id) const;
    bool active(uint64_t id) const;
    bool exists(uint64_t id) const { return pending_.contains(id); }
    void complete(const PluginMessage& message);
    void tick();
    void cancel_run(const std::string& run);
    void cancel_all();
    size_t active() const { return pending_.size(); }
    bool pending_for(const std::string& run) const;
};
}
