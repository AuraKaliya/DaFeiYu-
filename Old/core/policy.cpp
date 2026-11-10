#include "policy.hpp"
#include "capabilities.hpp"

namespace dfy {
// 初始化或恢复运行权限，并保留已持久化的撤销。
// 步骤：读取当前部署授权 → 新建策略或与已保存权限取交集 → 保存策略及版本。
PolicyGate::PolicyGate(StateStore& store, const Profile& profile)
    : store_(store), writer_(store.claim("policy", {"system/policy"})) {
    auto policy = store_.get("system", "policy");
    Json configured{{"actions", profile.data.at("allowed_actions")}, {"effects", profile.data.at("allowed_effects")},
        {"resources", profile.data.value("resources", Json::array({"*"}))}};

    if (policy.is_null()) {
        configured["revision"] = 1;
        writer_.save("system", "policy", configured, "policy.created");
    } else {
        const auto original = policy;
        // 部署配置可以收回授权；已持久化的撤销在重启后仍应保留。
        for (const auto* field : {"actions", "effects", "resources"}) {
            Json intersection = Json::array();
            for (const auto& value : policy.at(field))
                if (includes(configured.at(field), value))
                    intersection.push_back(value);
            policy[field] = intersection;
        }

        if (policy != original)
            policy["revision"] = policy.at("revision").get<int>() + 1;
        writer_.save("system", "policy", policy, "policy.restored");
    }
}

// 读取当前生效的实例权限策略。
Json PolicyGate::current() {
    return store_.get("system", "policy");
}

// 按期望版本更新权限集合，并保存新策略版本。
Json PolicyGate::revise(const Json& command) {
    auto value = current();
    require(value.at("revision") == command.at("expected_revision"), "POLICY_REVISION_CONFLICT");

    for (const auto* field : {"actions", "effects", "resources"})
        if (command.contains(field)) {
            require(command.at(field).is_array(), "POLICY_INVALID");
            value[field] = command.at(field);
        }
    value["revision"] = value.at("revision").get<int>() + 1;
    writer_.save("system", "policy", value, "policy.changed", value);
    return value;
}

namespace {
// 判断运行授权是否允许访问指定资源。
bool resource_allowed(const Json& grants, const std::string& resource) {
    for (const auto& item : grants.at("resources"))
        if (item == "*" || item == resource)
            return true;
    return false;
}
}

// 校验动作同时满足运行快照与当前策略，并验证输入契约。
// 步骤：核对动作及副作用授权 → 检查全部资源 → 校验动作输入。
void PolicyGate::action(const Json& run, const Json& descriptor, const Json& input) {
    const auto policy = current();
    const auto& grants = run.at("grants");
    require(includes(grants.at("actions"), descriptor.at("id")) && includes(policy.at("actions"), descriptor.at("id")),
        "ACTION_DENIED");
    require(includes(grants.at("effects"), descriptor.at("effects")) &&
                includes(policy.at("effects"), descriptor.at("effects")),
        "EFFECT_DENIED");

    for (const auto& resource : resources_of(descriptor))
        require(resource_allowed(grants, resource) && resource_allowed(policy, resource), "RESOURCE_DENIED");

    sdk::validate_value(descriptor.at("input"), input);
}

// 校验嵌套调用的委托边界、当前策略及操作身份。
// 步骤：复核根动作与委托副作用 → 要求写入关联操作 → 逐项检查委托和运行资源权限。
void PolicyGate::method(const CallScope& scope, const Json& descriptor) {
    const auto effects = descriptor.at("effects").get<std::string>();
    const auto policy = current();

    if (!scope.root_action.empty())
        require(includes(policy.at("actions"), scope.root_action), "NESTED_ACTION_REVOKED");
    require(includes(scope.delegation.at("effects"), effects), "NESTED_EFFECT_DENIED");

    if (effects != "read")
        require(!scope.operation.empty(), "OPERATION_HANDLE_REQUIRED");
    require(includes(scope.grants.at("effects"), effects) && includes(policy.at("effects"), effects), "EFFECT_DENIED");

    for (const auto& resource : resources_of(descriptor)) {
        require(permits(scope.delegation.at("resources"), resource), "NESTED_RESOURCE_DENIED");
        if (effects != "read")
            require(permits(scope.delegation.at("write_resources"), resource), "NESTED_RESOURCE_DENIED");
        require(resource_allowed(scope.grants, resource) && resource_allowed(policy, resource), "RESOURCE_DENIED");
    }
}

// 校验回复计划是否沿用任务授权目标及当前意图版本。
void PolicyGate::delivery(const Json& run, const Json& plan) {
    require(run.at("delivery_policy") == "reply" && !run.at("delivery_binding").is_null(), "DELIVERY_TARGET_DENIED");
    require(plan.at("binding") == run.at("delivery_binding") &&
                plan.at("target") == run.at("delivery_binding").at("target"),
        "DELIVERY_TARGET_DENIED");
    require(run.at("actor").is_string(), "DELIVERY_ACTOR_DENIED");
    require(plan.at("intent_revision") == run.at("intent_revision"), "DELIVERY_INTENT_CHANGED");
}

// 领取行为状态写权限，并在首次启动时设为可交互模式。
BehaviorController::BehaviorController(StateStore& store)
    : store_(store), writer_(store.claim("behavior", {"system/behavior", "activity_policy"})) {
    if (store_.get("system", "behavior").is_null())
        writer_.save(
            "system", "behavior", {{"mode", "available"}, {"revision", 1}, {"effective_until", 0}}, "behavior.created");
}

// 读取实例当前行为模式与生效期限。
Json BehaviorController::state() {
    return store_.get("system", "behavior");
}

// 按期望版本修改行为模式，并记录其期限和原因。
// 步骤：校验版本、模式及期限 → 构造新版本状态 → 持久化行为变化。
Json BehaviorController::transition(const Json& input) {
    auto value = state();
    require(input.at("expected_revision") == value.at("revision"), "BEHAVIOR_REVISION_CONFLICT");
    require(
        includes(Json::array({"available", "focused", "quiet", "resting"}), input.at("mode")), "BEHAVIOR_MODE_INVALID");

    const auto until = input.value("effective_until", int64_t{0});
    require(until == 0 || until > now_ms(), "BEHAVIOR_EXPIRY_INVALID");

    value = {{"mode", input.at("mode")}, {"revision", value.at("revision").get<int>() + 1}, {"effective_until", until},
        {"reason", input.value("reason", std::string{})}};
    writer_.save("system", "behavior", value, "behavior.changed", value);
    return value;
}

// 按期望版本更新活动启用状态。
Json BehaviorController::activity(const std::string& id, bool enabled, int revision) {
    auto value = activity_policy(id);
    require(!value.is_null(), "ACTIVITY_MISSING");
    require(value.at("policy_revision") == revision, "ACTIVITY_REVISION_CONFLICT");

    value["enabled"] = enabled;
    value["policy_revision"] = revision + 1;
    writer_.save("activity_policy", id, value, "activity.policy_changed", value);
    return value;
}

// 读取指定活动的启用策略及版本。
Json BehaviorController::activity_policy(const std::string& id) {
    return store_.get("activity_policy", id);
}

// 为尚无策略记录的活动保存初始启用状态。
void BehaviorController::ensure_activity(const std::string& id, bool enabled) {
    if (activity_policy(id).is_null())
        writer_.save("activity_policy", id, {{"id", id}, {"enabled", enabled}, {"policy_revision", 1}},
            "activity.policy_created");
}

// 在行为模式期限届满时恢复可交互状态。
void BehaviorController::tick() {
    auto value = state();
    if (value.at("effective_until").get<int64_t>() && now_ms() >= value.at("effective_until").get<int64_t>())
        (void)transition({{"expected_revision", value.at("revision")}, {"mode", "available"}, {"reason", "expired"}});
}

// 判断当前行为模式是否允许处理用户输入。
bool BehaviorController::user_ready() {
    return includes(Json::array({"available", "focused"}), state().at("mode"));
}
}
