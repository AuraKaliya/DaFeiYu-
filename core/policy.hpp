#pragma once
#include "calls.hpp"
namespace dfy {
class PolicyGate {
    StateStore& store_;
    StateWriter writer_;
public:
    PolicyGate(StateStore& store, const Profile& profile);
    Json current();
    Json revise(const Json& command);
    void action(const Json& run, const Json& descriptor, const Json& input);
    void method(const CallScope& scope, const Json& descriptor);
    void delivery(const Json& run, const Json& plan);
};
class BehaviorController {
    StateStore& store_;
    StateWriter writer_;
public:
    explicit BehaviorController(StateStore& store);
    Json state();
    Json transition(const Json& input);
    Json activity(const std::string& id, bool enabled, int revision);
    Json activity_policy(const std::string& id);
    void ensure_activity(const std::string& id, bool enabled);
    void tick();
    bool user_ready();
};
}
