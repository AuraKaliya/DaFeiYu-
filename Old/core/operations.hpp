#pragma once
#include "policy.hpp"
#include "conversations.hpp"
#include "scopes.hpp"

namespace dfy {
class OperationCoordinator {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    PolicyGate& policy_;
    Conversations& conversations_;
    ScopeManager& scopes_;
    int timeout_;
    using Done = std::function<void(const Json&, const Completion&)>;
    // 幂等准备操作记录，并阻止冲突计划或资源上未决副作用。
    Json prepare(const Json& run, const Json& descriptor, const Json& input, const std::string& key,
        const std::string& kind, const std::string& parent = {});
    // 将已准备操作派发给插件，并跟踪结算或未派发失败。
    void dispatch(Json operation, CallScope scope, Done done = {});
    // 归并操作完成证据，确定副作用已确认、未发生或仍待核对。
    void settle(const std::string& id, const Completion& completion, Done done = {});
    // 阻止向仍存在未知副作用的资源派发新的写操作。
    void blocked(const Json& descriptor, const std::string& except = {});

public:
    // 连接操作协调依赖，并领取操作记录的写权限。
    OperationCoordinator(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy,
        Conversations& conversations, ScopeManager& scopes, const Profile& profile);
    // 在权限和可用性满足时执行已登记动作。
    std::string action(const Json& run, const Json& descriptor, const Json& input, const std::string& key);
    // 校验回复授权并以稳定标识准备、派发渠道交付操作。
    std::string deliver(const Json& run, const Json& plan);
    // 协调插件发起的嵌套服务调用，并为写入建立子操作。
    void nested(const PluginMessage& message);
    // 将已注册后台作业绑定到仍在运行的所属操作。
    void attach_job(const std::string& operation, const std::string& job);

    // 将后台作业结果送入统一操作结算流程。
    void settle_job(const std::string& operation, const Completion& result) { settle(operation, result); }

    // 恢复中断操作，对无法确认的写入保留未知副作用状态。
    void recover();
    // 依据带版本和证据的核对命令结算未知副作用。
    Json reconcile(const Json& command);
    // 通过原服务查询操作结果，为未决副作用补充证据。
    void query(const std::string& id);
};

// 从运行任务快照构造服务调用的身份、授权和预算上下文。
CallScope run_scope(const Json& run, std::string role, std::string operation = {});
}
