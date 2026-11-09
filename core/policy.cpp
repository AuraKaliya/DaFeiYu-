#include "policy.hpp"
namespace dfy {
PolicyGate::PolicyGate(StateStore& store, const Profile& profile) : store_(store) {
    auto policy = store_.get("system", "policy");
    Json configured{{"actions", profile.data.at("allowed_actions")}, {"effects", profile.data.at("allowed_effects")},
        {"resources", profile.data.value("resources", Json::array({"*"}))}};
    if (policy.is_null()) {
        configured["revision"] = 1; store_.save("system", "policy", configured, "policy.created");
    } else {
        const auto original = policy;
        // Deployment configuration can remove grants; persisted revocations never disappear on restart.
        for (const auto* field : {"actions", "effects", "resources"}) {
            Json intersection = Json::array();
            for (const auto& value : policy.at(field)) if (includes(configured.at(field), value)) intersection.push_back(value);
            policy[field] = intersection;
        }
        if (policy != original) policy["revision"] = policy.at("revision").get<int>() + 1;
        store_.save("system", "policy", policy, "policy.restored");
    }
}
Json PolicyGate::current() { return store_.get("system", "policy"); }
Json PolicyGate::revise(const Json& command) {
    auto value = current(); require(value.at("revision") == command.at("expected_revision"), "POLICY_REVISION_CONFLICT");
    for (const auto* field : {"actions", "effects", "resources"}) if (command.contains(field)) { require(command.at(field).is_array(), "POLICY_INVALID"); value[field] = command.at(field); }
    value["revision"] = value.at("revision").get<int>() + 1;
    store_.save("system", "policy", value, "policy.changed", value); return value;
}
namespace {
bool resource_allowed(const Json& grants, const std::string& resource) {
    for (const auto& item : grants.at("resources")) if (item == "*" || item == resource) return true;
    return false;
}
int effect_rank(const std::string& effect) { return effect == "read" ? 0 : effect == "local-write" ? 1 : 2; }
}
void PolicyGate::action(const Json& run, const Json& descriptor, const Json& input) {
    const auto policy = current(); const auto& grants = run.at("grants");
    require(includes(grants.at("actions"), descriptor.at("id")) && includes(policy.at("actions"), descriptor.at("id")), "ACTION_DENIED");
    require(includes(grants.at("effects"), descriptor.at("effects")) && includes(policy.at("effects"), descriptor.at("effects")), "EFFECT_DENIED");
    require(resource_allowed(grants, descriptor.at("resource")) && resource_allowed(policy, descriptor.at("resource")), "RESOURCE_DENIED");
    sdk::validate_value(descriptor.at("input"), input);
}
void PolicyGate::method(const CallScope& scope, const Json& descriptor) {
    const auto effects = descriptor.at("effects").get<std::string>();
    require(effect_rank(effects) <= effect_rank(scope.effects), "NESTED_EFFECT_DENIED");
    if (effects == "read") {
        const auto policy = current();
        require(resource_allowed(scope.grants, descriptor.at("resource")) && resource_allowed(policy, descriptor.at("resource")), "RESOURCE_DENIED");
        return;
    }
    require(!scope.operation.empty(), "OPERATION_HANDLE_REQUIRED");
    require(scope.resource == descriptor.at("resource").get<std::string>(), "NESTED_RESOURCE_DENIED");
    const auto policy = current();
    require(includes(scope.grants.at("effects"), effects) && includes(policy.at("effects"), effects), "EFFECT_DENIED");
    require(resource_allowed(scope.grants, descriptor.at("resource")) && resource_allowed(policy, descriptor.at("resource")), "RESOURCE_DENIED");
}
void PolicyGate::delivery(const Json& run, const Json& plan) {
    require(run.at("delivery_policy") == "reply" && plan.at("target") == run.at("conversation"), "DELIVERY_TARGET_DENIED");
    require(run.at("actor").is_string(), "DELIVERY_ACTOR_DENIED");
    require(plan.at("intent_revision") == run.at("intent_revision"), "DELIVERY_INTENT_CHANGED");
}
BehaviorController::BehaviorController(StateStore& store) : store_(store) {
    if (store_.get("system", "behavior").is_null()) store_.save("system", "behavior",
        {{"mode", "available"}, {"revision", 1}, {"effective_until", 0}}, "behavior.created");
}
Json BehaviorController::state() { return store_.get("system", "behavior"); }
Json BehaviorController::transition(const Json& input) {
    auto value = state(); require(input.at("expected_revision") == value.at("revision"), "BEHAVIOR_REVISION_CONFLICT");
    require(includes(Json::array({"available", "focused", "quiet", "resting"}), input.at("mode")), "BEHAVIOR_MODE_INVALID");
    const auto until = input.value("effective_until", int64_t{0});
    require(until == 0 || until > now_ms(), "BEHAVIOR_EXPIRY_INVALID");
    value = {{"mode", input.at("mode")}, {"revision", value.at("revision").get<int>() + 1},
        {"effective_until", until}, {"reason", input.value("reason", std::string{})}};
    store_.save("system", "behavior", value, "behavior.changed", value); return value;
}
Json BehaviorController::activity(const std::string& id, bool enabled, int revision) {
    auto value = store_.get("activity", id); require(!value.is_null(), "ACTIVITY_MISSING");
    require(value.at("policy_revision") == revision, "ACTIVITY_REVISION_CONFLICT");
    value["enabled"] = enabled; value["policy_revision"] = revision + 1;
    store_.save("activity", id, value, "activity.policy_changed", value); return value;
}
void BehaviorController::tick() {
    auto value = state();
    if (value.at("effective_until").get<int64_t>() && now_ms() >= value.at("effective_until").get<int64_t>())
        (void)transition({{"expected_revision", value.at("revision")}, {"mode", "available"}, {"reason", "expired"}});
}
bool BehaviorController::user_ready() { return includes(Json::array({"available", "focused"}), state().at("mode")); }
}
