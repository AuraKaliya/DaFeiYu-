#pragma once
#include "plugins.hpp"
#include "store.hpp"
#include <dafeiyu/conversation.hpp>

namespace dfy {
class Conversations {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    const Profile& profile_;
    // 构造包含渠道身份和授权代次的会话路由及回复绑定。
    Json bind(const std::string& service, const std::string& target, Json authorization, const Json& reply_reference);

public:
    // 连接会话依赖，并领取会话及渠道授权状态的写权限。
    Conversations(StateStore& store, PluginRuntime& plugins, const Profile& profile)
        : store_(store), writer_(store.claim("conversations", {"conversation", "channel_authority"})),
          plugins_(plugins), profile_(profile) {}

    // 恢复渠道授权记录，并为撤销或变更后的渠道更新授权代次。
    void start();
    // 为本地所有者输入绑定指定或默认回复渠道。
    Json local(const std::string& target, const std::string& service = {});
    // 校验渠道输入来源，并绑定映射到实例所有者的回复身份。
    Json channel(const std::string& plugin, const std::string& service, const Json& input);
    // 为活动创建可回复的渠道会话或仅供内部使用的会话。
    Json activity(const std::string& id, const std::string& target, const std::string& reply_service = {});
    // 校验持久化交付绑定是否仍匹配当前渠道身份。
    void validate_identity(const Json& delivery) const;
    // 校验回复绑定的当前授权，阻止失效渠道或旧代次继续交付。
    void authorize(const Json& delivery);
    // 确保会话处于打开状态，重新打开时递增会话代次。
    Json ensure(const Json& route);
    // 关闭会话并递增代次，使已有会话作用域失效。
    Json close(const std::string& id);
};
}
