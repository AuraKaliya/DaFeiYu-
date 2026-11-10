#pragma once
#include "context.hpp"
#include "jobs.hpp"

namespace dfy {
class RunExecutor {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    PolicyGate& policy_;
    OperationCoordinator& operations_;
    JobCoordinator& jobs_;
    ContextAssembler& context_;
    const Profile& profile_;
    int timeout_;
    std::set<std::string> busy_;
    // 递增运行任务版本，并原子保存状态及对应事实。
    void save(Json run, const std::string& event, const Json& payload = Json::object());
    // 将未终止任务标记为失败或等待恢复，并记录错误。
    void fail(const std::string& id, const Json& error, bool wait = false);
    // 判断异步结果是否仍对应未终止任务的当前意图版本。
    bool current(const std::string& id, int intent) const;
    // 异步请求策略决策或收尾审核，并采纳仍适用的结果。
    void strategy(Json run, bool review);
    // 组装上下文并异步调用选定模型，将有效输出转换为策略候选。
    void model(Json run, const Json& proposal);
    // 记录模型尝试失败，并按错误类型终止、等待或交回策略处理。
    void model_failed(const std::string& id, const std::string& attempt, const Json& error, const std::string& stage);
    // 采纳策略提案并持久化策略状态和决策记录。
    void adopt(Json run, const Json& result, const std::string& decision);
    // 保存策略给出的目标结论与回答，并将运行任务置为完成。
    void complete(Json run);

public:
    // 连接任务执行依赖，并领取运行任务、策略决策和模型尝试的写权限。
    RunExecutor(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy,
        OperationCoordinator& operations, JobCoordinator& jobs, ContextAssembler& context, const Profile& profile);

    // 判断是否仍有任务占用策略或模型处理阶段。
    bool busy() const { return !busy_.empty(); }

    // 根据终态、忙碌标记、等待操作或唤醒时间判断任务能否推进。
    bool runnable(const Json& run);
    // 按持久化阶段推进运行任务，协调决策、动作、等待和交付。
    void advance(Json run);
    // 将中断运行任务恢复到可重新决策或派发准备操作的阶段。
    void recover();
    // 终止运行任务，并请求取消其服务调用和后台作业。
    Json cancel(const std::string& id);
    // 用新输入修订运行意图，使旧调用结果失去业务推进资格。
    Json steer(const std::string& id, const Json& input);
    // 在绑定和在途调用条件允许时重新排队运行任务。
    Json resume(const std::string& id);
    // 从已接收输入创建携带授权和绑定快照的运行任务。
    std::string create(const Json& input, const Json& grants);
    // 将收到新结算证据的等待任务重新加入可调度队列。
    void operation_changed(const std::string& operation);
    // 为已终止的活动任务幂等记录结算反馈完成状态。
    void feedback_completed(const std::string& id, const Json& result);
    // 将运行任务状态和调用预算转换为公开决策视图。
    Json view(const std::string& id);
    // 分页读取运行任务，并逐项转换为公开视图。
    Json views(uint64_t cursor, size_t limit);
    // 取消指定会话中尚未终止的运行任务。
    void cancel_conversation(const std::string& id);
};
}
