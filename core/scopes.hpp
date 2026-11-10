#pragma once
#include "policy.hpp"
#include "conversations.hpp"

namespace dfy {
class ScopeManager {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    PolicyGate& policy_;
    Conversations& conversations_;
    const Profile& profile_;
    const std::string epoch_ = identifier();
    std::map<std::string, Json> scopes_, registrations_;
    // 取得插件获准申请的指定作用域权限。
    Json permissions(const std::string& plugin, const std::string& kind) const;
    // 判断作用域是否仍符合配置、权限、代次和来源授权。
    bool current(const Json& scope);
    // 生成对插件公开的作用域令牌、主体及代次描述。
    Json descriptor(const Json& scope) const;
    // 为有效作用域生成本次进程令牌，并保存激活或恢复状态。
    std::string activate(Json scope, bool restored);
    // 关闭或挂起作用域，并同步处理注册和在途调用。
    void close_impl(const std::string& id, const std::string& reason, bool suspend);

public:
    std::function<void(const Json&)> released;

    // 连接作用域依赖，并领取作用域及资源注册记录的写权限。
    ScopeManager(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy,
        Conversations& conversations, const Profile& profile)
        : store_(store), writer_(store.claim("scopes", {"scope", "registration"})), plugins_(plugins), calls_(calls),
          policy_(policy), conversations_(conversations), profile_(profile) {}

    // 恢复仍有效的持久化作用域及注册，并重新签发临时令牌。
    void restore();
    // 在配置授权和父调用边界内创建或复用作用域。
    Json open(const std::string& plugin, const Json& request);
    // 取得属于指定插件且当前仍有效的作用域。
    Json owned(const std::string& plugin, const std::string& id);
    // 判断作用域令牌是否存在且仍然有效。
    bool valid(const std::string& id);
    // 在作用域内幂等登记上下文、动作可见性或事实订阅。
    Json register_resource(const std::string& plugin, const std::string& scope, const std::string& key,
        const std::string& kind, const Json& value);
    // 释放插件所属的资源注册，并撤销依赖该注册的调用。
    Json release(const std::string& plugin, const std::string& id);
    // 核对所有者后显式关闭作用域，重复关闭返回未关闭标记。
    Json close(const std::string& plugin, const std::string& id);
    // 查找失效作用域并统一关闭其关联资源。
    void refresh();
    // 在实例停止时关闭调用作用域，并挂起可恢复作用域。
    void shutdown();
    // 在作用域授权内提交只读服务调用，并限制回调结果的使用期限。
    void request(const PluginMessage& message);
    // 按作用域边界读取运行视图、会话或行为状态。
    Json read_state(const std::string& plugin, const std::string& scope, const std::string& object);
    // 读取活动资源注册，令牌缺失时返回空值。
    Json registration(const std::string& token) const;

    // 汇总当前已激活的资源注册记录。
    Json registrations() const {
        Json result = Json::array();
        for (const auto& [id, item] : registrations_) {
            (void)id;
            result.push_back(item);
        }
        return result;
    }

    // 判断资源注册及其所属作用域是否均有效。
    bool registration_active(const std::string& token);
    // 根据实例、会话或任务边界判断作用域是否覆盖运行任务。
    bool contains(const Json& scope, const Json& run) const;
    // 为限定作用域的动作查找覆盖当前任务的有效注册。
    std::string action_registration(const Json& action, const Json& run);
    // 收集覆盖当前运行任务的有效上下文服务注册。
    std::vector<std::pair<std::string, std::string>> contexts(const Json& run);

    // 返回当前活动作用域和资源注册数量。
    Json metrics() const { return {{"scopes", scopes_.size()}, {"registrations", registrations_.size()}}; }
};
}
