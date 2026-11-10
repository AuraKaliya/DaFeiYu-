#pragma once
#include "calls.hpp"

namespace dfy {
class PolicyGate {
    StateStore& store_;
    StateWriter writer_;

public:
    // 初始化或恢复运行权限，并保留已持久化的撤销。
    PolicyGate(StateStore& store, const Profile& profile);
    // 读取当前生效的实例权限策略。
    Json current();
    // 按期望版本更新权限集合，并保存新策略版本。
    Json revise(const Json& command);
    // 校验动作同时满足运行快照与当前策略，并验证输入契约。
    void action(const Json& run, const Json& descriptor, const Json& input);
    // 校验嵌套调用的委托边界、当前策略及操作身份。
    void method(const CallScope& scope, const Json& descriptor);
    // 校验回复计划是否沿用任务授权目标及当前意图版本。
    void delivery(const Json& run, const Json& plan);
};

class BehaviorController {
    StateStore& store_;
    StateWriter writer_;

public:
    // 领取行为状态写权限，并在首次启动时设为可交互模式。
    explicit BehaviorController(StateStore& store);
    // 读取实例当前行为模式与生效期限。
    Json state();
    // 按期望版本修改行为模式，并记录其期限和原因。
    Json transition(const Json& input);
    // 按期望版本更新活动启用状态。
    Json activity(const std::string& id, bool enabled, int revision);
    // 读取指定活动的启用策略及版本。
    Json activity_policy(const std::string& id);
    // 为尚无策略记录的活动保存初始启用状态。
    void ensure_activity(const std::string& id, bool enabled);
    // 在行为模式期限届满时恢复可交互状态。
    void tick();
    // 判断当前行为模式是否允许处理用户输入。
    bool user_ready();
};
}
