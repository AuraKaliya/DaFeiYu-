#include "policy.hpp"
#include "capabilities.hpp"
namespace dfy {
PolicyGate::PolicyGate(StateStore& store, const Profile& profile) : store_(store), writer_(store.claim("policy", {"system/policy"})) {
    auto policy = store_.get("system", "policy");
    Json configured{{"actions", profile.data.at("allowed_actions")}, {"effects", profile.data.at("allowed_effects")},
        {"resources", profile.data.value("resources", Json::array({"*"}))}};
    if (policy.is_null()) {
        configured["revision"] = 1; writer_.save("system", "policy", configured, "policy.created");
    } else {
        const auto original = policy;
        // Deployment configuration can remove grants; persisted revocations never disappear on restart.
        for (const auto* field : {"actions", "effects", "resources"}) {
            Json intersection = Json::array();
            for (const auto& value : policy.at(field)) if (includes(configured.at(field), value)) intersection.push_back(value);
            policy[field] = intersection;
        }
        if (policy != original) policy["revision"] = policy.at("revision").get<int>() + 1;
        writer_.save("system", "policy", policy, "policy.restored");
    }
}
Json PolicyGate::current() { return store_.get("system", "policy"); }
Json PolicyGate::revise(const Json& command) {
    auto value = current(); require(value.at("revision") == command.at("expected_revision"), "POLICY_REVISION_CONFLICT");
    for (const auto* field : {"actions", "effects", "resources"}) if (command.contains(field)) { require(command.at(field).is_array(), "POLICY_INVALID"); value[field] = command.at(field); }
    value["revision"] = value.at("revision").get<int>() + 1;
    writer_.save("system", "policy", value, "policy.changed", value); return value;
}
namespace {
bool resource_allowed(const Json& grants, const std::string& resource) {
    for (const auto& item : grants.at("resources")) if (item == "*" || item == resource) return true;
    return false;
}
}
void PolicyGate::action(const Json& run, const Json& descriptor, const Json& input) {
    const auto policy = current(); const auto& grants = run.at("grants");
    require(includes(grants.at("actions"), descriptor.at("id")) && includes(policy.at("actions"), descriptor.at("id")), "ACTION_DENIED");
    require(includes(grants.at("effects"), descriptor.at("effects")) && includes(policy.at("effects"), descriptor.at("effects")), "EFFECT_DENIED");
    for (const auto& resource : resources_of(descriptor))
        require(resource_allowed(grants, resource) && resource_allowed(policy, resource), "RESOURCE_DENIED");
    sdk::validate_value(descriptor.at("input"), input);
}
void PolicyGate::method(const CallScope& scope, const Json& descriptor) {
    const auto effects = descriptor.at("effects").get<std::string>();
    const auto policy = current();
    if (!scope.root_action.empty()) require(includes(policy.at("actions"), scope.root_action), "NESTED_ACTION_REVOKED");
    require(includes(scope.delegation.at("effects"), effects), "NESTED_EFFECT_DENIED");
    if (effects != "read") require(!scope.operation.empty(), "OPERATION_HANDLE_REQUIRED");
    require(includes(scope.grants.at("effects"), effects) && includes(policy.at("effects"), effects), "EFFECT_DENIED");
    for (const auto& resource : resources_of(descriptor)) {
        require(permits(scope.delegation.at("resources"), resource), "NESTED_RESOURCE_DENIED");
        if (effects != "read") require(permits(scope.delegation.at("write_resources"), resource), "NESTED_RESOURCE_DENIED");
        require(resource_allowed(scope.grants, resource) && resource_allowed(policy, resource), "RESOURCE_DENIED");
    }
}
void PolicyGate::delivery(const Json& run, const Json& plan) {
    require(run.at("delivery_policy") == "reply" && !run.at("delivery_binding").is_null(), "DELIVERY_TARGET_DENIED");
    require(plan.at("binding") == run.at("delivery_binding") && plan.at("target") == run.at("delivery_binding").at("target"), "DELIVERY_TARGET_DENIED");
    require(run.at("actor").is_string(), "DELIVERY_ACTOR_DENIED");
    require(plan.at("intent_revision") == run.at("intent_revision"), "DELIVERY_INTENT_CHANGED");
}
BehaviorController::BehaviorController(StateStore& store) : store_(store), writer_(store.claim("behavior", {"system/behavior", "activity_policy"})) {
    if (store_.get("system", "behavior").is_null()) writer_.save("system", "behavior",
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
    writer_.save("system", "behavior", value, "behavior.changed", value); return value;
}
Json BehaviorController::activity(const std::string& id, bool enabled, int revision) {
    auto value = activity_policy(id); require(!value.is_null(), "ACTIVITY_MISSING");
    require(value.at("policy_revision") == revision, "ACTIVITY_REVISION_CONFLICT");
    value["enabled"] = enabled; value["policy_revision"] = revision + 1;
    writer_.save("activity_policy", id, value, "activity.policy_changed", value); return value;
}
Json BehaviorController::activity_policy(const std::string& id) { return store_.get("activity_policy", id); }
void BehaviorController::ensure_activity(const std::string& id, bool enabled) {
    if (activity_policy(id).is_null()) writer_.save("activity_policy", id, {{"id", id}, {"enabled", enabled}, {"policy_revision", 1}}, "activity.policy_created");
}
void BehaviorController::tick() {
    auto value = state();
    if (value.at("effective_until").get<int64_t>() && now_ms() >= value.at("effective_until").get<int64_t>())
        (void)transition({{"expected_revision", value.at("revision")}, {"mode", "available"}, {"reason", "expired"}});
}
bool BehaviorController::user_ready() { return includes(Json::array({"available", "focused"}), state().at("mode")); }
}
