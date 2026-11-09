#pragma once
#include "run.hpp"
#include "conversations.hpp"
namespace dfy {
class InboxRouter {
    StateStore& store_;
    PluginRuntime& plugins_;
    PolicyGate& policy_;
    BehaviorController& behavior_;
    RunExecutor& runs_;
    Conversations& conversations_;
    const Profile& profile_;
    Json accept_bound(const std::string& source, const std::string& key, Json input, const Json& route);
public:
    InboxRouter(StateStore& store, PluginRuntime& plugins, PolicyGate& policy, BehaviorController& behavior, RunExecutor& runs, Conversations& conversations, const Profile& profile)
        : store_(store), plugins_(plugins), policy_(policy), behavior_(behavior), runs_(runs), conversations_(conversations), profile_(profile) {}
    Json accept_local(const std::string& key, Json input, const std::string& channel = {});
    Json accept_channel(const std::string& plugin, const std::string& channel, const std::string& key, Json input);
    Json accept_activity(const std::string& id, const std::string& key, Json input, const std::string& channel = {});
    Json steer_local(const std::string& key, const Json& run, const std::string& text);
    void route();
};
class Scheduler {
    StateStore& store_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    BehaviorController& behavior_;
    InboxRouter& inbox_;
    RunExecutor& runs_;
    const Profile& profile_;
    std::set<std::string> evaluating_, notifying_;
    void trigger(Json activity, const std::string& key, const Json& fact);
public:
    Scheduler(StateStore& store, PluginRuntime& plugins, CallRouter& calls, BehaviorController& behavior, InboxRouter& inbox, RunExecutor& runs, const Profile& profile)
        : store_(store), plugins_(plugins), calls_(calls), behavior_(behavior), inbox_(inbox), runs_(runs), profile_(profile) {}
    void start();
    void tick();
};
}
