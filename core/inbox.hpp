#pragma once
#include "run.hpp"
namespace dfy {
class InboxRouter {
    StateStore& store_;
    PluginRuntime& plugins_;
    PolicyGate& policy_;
    BehaviorController& behavior_;
    RunExecutor& runs_;
    const Profile& profile_;
public:
    InboxRouter(StateStore& store, PluginRuntime& plugins, PolicyGate& policy, BehaviorController& behavior, RunExecutor& runs, const Profile& profile)
        : store_(store), plugins_(plugins), policy_(policy), behavior_(behavior), runs_(runs), profile_(profile) {}
    Json accept(const std::string& source, const std::string& key, Json input);
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
