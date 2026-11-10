#pragma once
#include "common.hpp"
#include <memory>
#include <optional>

namespace dfy {
struct PluginMessage {
    std::string plugin;
    Json body;
};

class PluginRuntime {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    using Receiver = std::function<int(PluginMessage)>;
    // 建立插件管理实现并保存实例装配依赖。
    PluginRuntime(const Profile& profile, const Path& state, const std::string& instance, Receiver receive);
    // 停止由当前运行时管理的全部插件。
    ~PluginRuntime();
    // 按配置和依赖关系加载插件，校验契约并登记可用服务及动作。
    void start();
    // 停止接收新调用，并通知各插件停用及取消在途工作。
    void quiesce();
    // 进入停用状态，并按依赖逆序关闭插件对象。
    void stop();
    // 汇总插件状态、启动诊断和声明的服务目录。
    Json catalog() const;
    // 返回已经成功登记的动作目录。
    Json actions() const;
    // 收集就绪插件声明的活动，并校验活动名称和服务归属。
    Json activities() const;
    // 取得已登记服务的描述符，缺失时报告错误。
    Json service(const std::string& id) const;
    // 取得已登记动作的描述符，缺失时报告错误。
    Json action(const std::string& id) const;
    // 取得服务中指定方法的契约描述。
    Json method(const std::string& service, const std::string& method) const;
    // 解析配置中的角色或依赖绑定，缺失时报告错误。
    std::string binding(const std::string& name) const;
    // 取得提供指定服务的插件标识。
    std::string owner(const std::string& service) const;
    // 判断指定服务的提供插件是否就绪。
    bool ready(const std::string& service) const;
    // 判断插件是否声明了对指定绑定的依赖。
    bool dependency(const std::string& plugin, const std::string& binding) const;
    // 读取指定插件的部署配置与授权声明。
    Json selection(const std::string& plugin) const;
    // 判断指定插件是否存在且处于就绪状态。
    bool plugin_ready(const std::string& plugin) const;
    // 生成插件自身及其递归依赖的服务绑定快照。
    Json plugin_snapshot(const std::string& plugin) const;
    // 列出声明渠道契约的服务。
    Json channels() const;
    // 校验并返回渠道的实例、账号和提供者配置身份。
    Json channel_binding(const std::string& service) const;
    // 冻结运行任务允许使用的角色、动作及递归依赖。
    Json snapshot(const Json& action_ids, std::optional<std::string> channel = std::nullopt) const;
    // 核对已保存快照中的所有者与服务定义是否仍匹配部署。
    bool matches(const Json& snapshot) const;
    // 登记有期限的插件调用，并将调用事件投入插件队列。
    void invoke(uint64_t call, const std::string& service, const std::string& method, const Json& input,
        const Json& context, SteadyClock::time_point deadline);
    // 在插件工作线程投递事件，并将处理失败转换为核心消息。
    void send(const std::string& plugin, const Json& event);
    // 为指定调用设置协作式取消标记。
    void cancel(uint64_t call);
    // 释放指定调用在各插件中的跟踪记录。
    void release(uint64_t call);
    // 允许指定插件向核心回传已登记后台作业的结果。
    void allow_job(const std::string& plugin, const std::string& job);
};
}
