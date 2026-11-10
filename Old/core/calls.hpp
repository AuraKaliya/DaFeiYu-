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
    std::string scope_id, registration_id, conversation;
    bool scoped_request{};
    Json action_definition;
    // 生成传递给插件的调用身份、意图版本及授权上下文。
    Json context() const;
};

struct Completion {
    Json result;
    Json error;
    bool late{};
    bool physical{true};
};

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
    StateWriter writer_;
    uint64_t next_{1};
    std::map<uint64_t, PendingCall> pending_;
    const std::string epoch_ = identifier();
    // 逻辑终止整棵子调用树，并保留物理结果的后续跟踪。
    void terminate(uint64_t root, const std::string& code);

public:
    std::function<bool(const CallScope&)> scope_valid = [](const CallScope&) {
        return true;
    };

    // 连接插件运行时和状态库，并领取调用预算的写权限。
    CallRouter(PluginRuntime& plugins, StateStore& store)
        : plugins_(plugins), store_(store), writer_(store.claim("calls", {"call_budget"})) {}

    // 校验并派发服务调用，约束调用树的权限、期限和共享预算。
    uint64_t request(const std::string& service, const std::string& method, const Json& input, CallScope scope,
        int timeout_ms, std::function<void(const Completion&)> done);
    // 取得指定插件仍有权使用的活动调用记录。
    const PendingCall& owned(const std::string& plugin, uint64_t id) const;
    // 判断调用是否仍具备有效作用域、期限和逻辑执行资格。
    bool active(uint64_t id) const;

    // 判断调用是否仍保留物理完成跟踪记录。
    bool exists(uint64_t id) const { return pending_.contains(id); }

    // 接收插件的物理完成消息并向调用方交付校验后的结果。
    void complete(const PluginMessage& message);
    // 找出超过期限的调用并逻辑终止对应调用树。
    void tick();
    // 逻辑终止指定运行任务的全部调用树。
    void cancel_run(const std::string& run);
    // 逻辑终止当前跟踪的全部调用树。
    void cancel_all();
    // 逻辑终止依赖指定作用域的调用树。
    void cancel_scope(const std::string& scope);
    // 逻辑终止依赖指定资源注册的调用树。
    void cancel_registration(const std::string& registration);
    // 取得尚未逻辑终止的调用中最早的截止时间。
    SteadyClock::time_point next_deadline() const;

    // 返回仍在跟踪的调用数量，包含等待物理完成的逻辑终止调用。
    size_t active() const { return pending_.size(); }

    // 判断运行任务是否仍有等待物理完成的调用。
    bool pending_for(const std::string& run) const;
};
}
