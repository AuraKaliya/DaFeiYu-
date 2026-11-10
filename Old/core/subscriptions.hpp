#pragma once
#include "scopes.hpp"

namespace dfy {
class Subscriptions {
    StateStore& store_;
    StateWriter writer_;
    ScopeManager& scopes_;
    PluginRuntime& plugins_;
    std::map<std::string, Json> active_;
    uint64_t observed_{};
    bool dirty_{true};

public:
    // 连接事实订阅依赖，并领取订阅状态的写权限。
    Subscriptions(StateStore& store, ScopeManager& scopes, PluginRuntime& plugins)
        : store_(store), writer_(store.claim("subscriptions", {"subscription"})), scopes_(scopes), plugins_(plugins) {}

    // 从有效资源注册恢复订阅，并允许重送尚未确认的事件页。
    // 步骤：筛选订阅注册 → 读取持久化游标并清除待确认页 → 保存恢复状态并关联新令牌。
    void restore() {
        for (const auto& registration : scopes_.registrations())
            if (registration.at("kind") == "subscription") {
                auto value = store_.get("subscription", registration.at("id"));
                if (value.is_null())
                    continue;

                value["state"] = "active";
                value["pending"] = nullptr;
                store_.atomic([&] { writer_.put("subscription", value.at("id"), value); });

                active_[registration.at("_token")] = value;
            }
    }

    // 校验观察权限并幂等登记作用域内的事实订阅。
    // 步骤：核对事件类型和过滤范围 → 注册作用域资源 → 复用或创建订阅游标 → 激活订阅。
    Json subscribe(const std::string& plugin, const Json& body) {
        const auto scope = scopes_.owned(plugin, body.at("scope_id"));
        const auto types = body.at("types"), selected = plugins_.selection(plugin);

        require(types.is_array() && !types.empty(), "SUBSCRIPTION_FILTER_REQUIRED");
        for (const auto& type : types)
            require(includes(selected.value("observe", Json::array()), type), "SUBSCRIPTION_DENIED");

        std::string run = scope.at("run"), conversation = scope.at("conversation");
        const auto filter = body.value("filter", Json::object());
        if (filter.contains("run_id")) {
            const auto target = store_.get("run", filter.at("run_id"));
            require(!target.is_null() && scopes_.contains(scope, target), "SUBSCRIPTION_SCOPE_DENIED");
            run = filter.at("run_id");
        }
        if (filter.contains("conversation_id")) {
            const auto target = filter.at("conversation_id").get<std::string>();
            require(conversation.empty() || conversation == target, "SUBSCRIPTION_SCOPE_DENIED");
            conversation = target;
        }

        const Json definition{{"types", types}, {"run_id", run}, {"conversation_id", conversation}};
        const auto registration =
            scopes_.register_resource(plugin, body.at("scope_id"), body.at("key"), "subscription", definition);
        const std::string id = registration.at("persistent_id"), token = registration.at("id");
        auto value = store_.get("subscription", id);

        if (value.is_null())
            value = {{"id", id}, {"plugin", plugin}, {"key", body.at("key")}, {"definition", definition}, {"cursor", 0},
                {"sent", 0}, {"pending", nullptr}};
        require(value.at("definition") == definition, "SUBSCRIPTION_CHANGED");

        value["state"] = "active";
        store_.atomic([&] { writer_.put("subscription", id, value); });
        active_[token] = value;
        dirty_ = true;
        return registration;
    }

    // 在注册释放或挂起时移除活动订阅并保存对应状态。
    void release(const Json& registration) {
        if (registration.at("kind") != "subscription")
            return;
        const auto token = registration.value("_token", std::string{});

        auto value = store_.get("subscription", registration.at("id"));
        active_.erase(token);
        if (value.is_null())
            return;

        value["state"] = registration.at("state");
        store_.atomic([&] { writer_.put("subscription", value.at("id"), value); });
    }

    // 校验插件对已发送事件页的确认，并持久化消费游标。
    // 步骤：检查注册有效性及插件归属 → 核对已发送序号 → 保存消费游标并允许发送下一页。
    void acknowledge(const std::string& plugin, const Json& body) {
        const auto token = body.at("registration_id").get<std::string>();
        require(active_.contains(token) && scopes_.registration_active(token), "SUBSCRIPTION_CLOSED");
        auto& value = active_.at(token);
        require(value.at("plugin") == plugin, "SUBSCRIPTION_OWNER_DENIED");

        require(body.at("sequence") == value.at("sent"), "SUBSCRIPTION_ACK_GAP");
        value["cursor"] = body.at("sequence");
        value["pending"] = nullptr;
        store_.atomic([&] { writer_.put("subscription", value.at("id"), value); });
        dirty_ = true;
    }

    // 向有效订阅发送下一页事实，并等待显式确认后继续。
    // 步骤：检查日志变化 → 跳过失效或待确认订阅 → 分页读取并保存发送状态 → 向插件推送事件页。
    void tick() {
        if (!dirty_ && observed_ == store_.sequence())
            return;
        observed_ = store_.sequence();
        dirty_ = false;

        for (auto& [token, value] : active_) {
            if (!scopes_.registration_active(token) || !value.at("pending").is_null())
                continue;
            const auto& definition = value.at("definition");

            auto page = store_.events(value.at("cursor"), 32, definition.at("run_id"), definition.at("types"),
                definition.at("conversation_id"));
            if (page.at("cursor") == value.at("cursor"))
                continue;
            if (page.at("events").empty())
                value["cursor"] = page.at("cursor");
            else {
                value["sent"] = page.at("cursor");
                value["pending"] = page;
            }

            store_.atomic([&] { writer_.put("subscription", value.at("id"), value); });
            if (!page.at("events").empty())
                plugins_.send(value.at("plugin"),
                    {{"type", "facts"}, {"key", value.at("key")}, {"registration_id", token}, {"page", page}});
        }
    }
};
}
