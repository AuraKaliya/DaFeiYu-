#pragma once
#include "operations.hpp"

namespace dfy {
class JobCoordinator {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    OperationCoordinator& operations_;
    int timeout_, max_jobs_;

public:
    // 连接后台作业依赖，并领取作业写权限及读取容量限制。
    JobCoordinator(StateStore& store, PluginRuntime& plugins, CallRouter& calls, OperationCoordinator& operations,
        const Profile& profile);
    // 为动作调用登记可恢复的后台作业，并返回持久化注册回执。
    void register_job(const PluginMessage& message);
    // 幂等接收后台作业的终态结果，并同步结算所属操作。
    void job_result(const PluginMessage& message);
    // 记录运行任务的作业取消请求，并尝试通知执行插件。
    void cancel_jobs(const std::string& run);
    // 恢复未完成后台作业的结果查询与观察连接。
    void recover();
};
}
