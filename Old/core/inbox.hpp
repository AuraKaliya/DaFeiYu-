#pragma once
#include "run.hpp"
#include "conversations.hpp"

namespace dfy {
class InboxRouter {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    PolicyGate& policy_;
    BehaviorController& behavior_;
    RunExecutor& runs_;
    Conversations& conversations_;
    const Profile& profile_;
    // 校验并幂等接收已绑定路由的输入，持久化接收回执。
    Json accept_bound(const std::string& source, const std::string& key, Json input, const Json& route);

public:
    // 连接输入接入依赖，并领取输入记录的写权限。
    InboxRouter(StateStore& store, PluginRuntime& plugins, PolicyGate& policy, BehaviorController& behavior,
        RunExecutor& runs, Conversations& conversations, const Profile& profile)
        : store_(store), writer_(store.claim("inbox", {"input"})), plugins_(plugins), policy_(policy),
          behavior_(behavior), runs_(runs), conversations_(conversations), profile_(profile) {}

    // 将本地输入归属实例所有者，并按本地回复路由接收入箱。
    Json accept_local(const std::string& key, Json input, const std::string& channel = {});
    // 规范化渠道输入并以渠道身份隔离输入去重范围。
    Json accept_channel(const std::string& plugin, const std::string& channel, const std::string& key, Json input);
    // 为活动输入绑定会话，并交由统一入箱流程持久化。
    Json accept_activity(const std::string& id, const std::string& key, Json input, const std::string& channel = {});
    // 沿用运行任务的会话与交付绑定，接收本地意图修订输入。
    Json steer_local(const std::string& key, const Json& run, const std::string& text);
    // 将待处理输入转换为新运行任务或已有任务的意图修订。
    void route();
};

class Scheduler {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    BehaviorController& behavior_;
    InboxRouter& inbox_;
    RunExecutor& runs_;
    const Profile& profile_;
    std::set<std::string> evaluating_, notifying_;
    std::map<std::string, Json> definitions_;
    // 评估活动触发条件，并将通过评估的建议转换为输入。
    void trigger(Json activity, const std::string& key, const Json& fact);

public:
    // 连接调度依赖，并领取活动定义和触发记录的写权限。
    Scheduler(StateStore& store, PluginRuntime& plugins, CallRouter& calls, BehaviorController& behavior,
        InboxRouter& inbox, RunExecutor& runs, const Profile& profile)
        : store_(store), writer_(store.claim("scheduler", {"activity", "occurrence"})), plugins_(plugins),
          calls_(calls), behavior_(behavior), inbox_(inbox), runs_(runs), profile_(profile) {}

    // 登记配置中的活动，并恢复或退役持久化活动定义。
    void start();
    // 按行为模式推进活动触发、用户任务及活动结算反馈。
    void tick();
    // 合并活动记录及其当前启用策略，生成查询结果。
    Json activity_views();
    // 取得活动、任务等待及行为模式期限中的下次唤醒时间。
    int64_t next_wakeup();
};
}
