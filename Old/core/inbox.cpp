#include "inbox.hpp"

namespace dfy {
// 将本地输入归属实例所有者，并按本地回复路由接收入箱。
Json InboxRouter::accept_local(const std::string& key, Json input, const std::string& channel) {
    input["actor"] = profile_.data.at("owner");
    input["delivery_policy"] = "reply";
    const auto route = conversations_.local(input.at("conversation"), channel);
    return accept_bound("local", key, std::move(input), route);
}

// 规范化渠道输入并以渠道身份隔离输入去重范围。
// 步骤：校验并绑定渠道 → 设置所有者及清除活动元数据 → 构造来源标识并接收入箱。
Json InboxRouter::accept_channel(
    const std::string& plugin, const std::string& channel, const std::string& key, Json input) {
    const auto route = conversations_.channel(plugin, channel, input);

    input["actor"] = profile_.data.at("owner");
    input["delivery_policy"] = "reply";
    input.erase("activity");
    input.erase("auto_depth");

    const auto source =
        Json::array({plugin, channel, route.at("key").at("channel_instance"), route.at("key").at("account")}).dump();
    return accept_bound(source, key, std::move(input), route);
}

// 为活动输入绑定会话，并交由统一入箱流程持久化。
Json InboxRouter::accept_activity(
    const std::string& id, const std::string& key, Json input, const std::string& channel) {
    const auto route = conversations_.activity(id, input.at("conversation"), channel);
    return accept_bound("activity", key, std::move(input), route);
}

// 沿用运行任务的会话与交付绑定，接收本地意图修订输入。
Json InboxRouter::steer_local(const std::string& key, const Json& run, const std::string& text) {
    const Json route{
        {"id", run.at("conversation")}, {"key", run.at("conversation_key")}, {"delivery", run.at("delivery_binding")}};
    return accept_bound("local", key,
        {{"run_id", run.at("id")}, {"text", text}, {"actor", profile_.data.at("owner")},
            {"conversation", run.at("external_conversation")}},
        route);
}

// 校验并幂等接收已绑定路由的输入，持久化接收回执。
// 步骤：校验输入 → 复用相同来源和键的回执 → 检查入箱容量 → 事务保存会话代次、输入及事实。
Json InboxRouter::accept_bound(const std::string& source, const std::string& key, Json input, const Json& route) {
    require(!key.empty() && key.size() <= 160, "INPUT_KEY_INVALID");
    require(input.at("text").is_string() && input.at("text").get<std::string>().size() <= 65536, "INPUT_TEXT_INVALID");
    require(input.at("actor") == profile_.data.at("owner"), "INPUT_ACTOR_DENIED");
    require(input.at("conversation").is_string(), "INPUT_CONVERSATION_INVALID");

    const auto identity = source + ":" + key;
    auto previous = store_.get("input", identity);
    if (!previous.is_null()) {
        require(previous.at("input") == input && previous.at("route") == route, "INPUT_KEY_CONFLICT");
        return previous;
    }

    const size_t pending = store_.pending_input_count();
    require(pending < profile_.data.at("limits").at("max_inbox").get<size_t>(), "INBOX_FULL");

    Json value{{"id", identity}, {"source", source}, {"key", key}, {"input", input}, {"route", route},
        {"state", "accepted"}, {"received_at", now_ms()}, {"run_id", nullptr}};

    store_.atomic([&] {
        value["conversation_generation"] = conversations_.ensure(route).at("generation");
        writer_.put("input", identity, value);
        writer_.event("input.accepted", value, {}, "core", {}, route.at("id"));
    });
    return value;
}

// 将待处理输入转换为新运行任务或已有任务的意图修订。
// 步骤：校验会话与行为准入 → 按输入类型修订任务或计算授权 → 事务关联任务并消费输入 → 记录拒绝原因。
void InboxRouter::route() {
    for (auto input : store_.pending_inputs(profile_.data.at("limits").at("max_inbox"))) {
        if (input.at("state") != "accepted")
            continue;
        try {
            const auto& payload = input.at("input");
            const auto conversation = store_.get("conversation", input.at("route").at("id"));
            require(conversation.at("state") == "open" &&
                        conversation.at("generation") == input.at("conversation_generation"),
                "CONVERSATION_CLOSED");
            const auto activity = payload.value("activity", std::string{});
            if (activity.empty() && !behavior_.user_ready())
                continue;
            if (!activity.empty()) {
                const auto state = store_.get("activity", activity);
                if (state.is_null() || !behavior_.activity_policy(activity).at("enabled").get<bool>() ||
                    !includes(state.at("definition").at("allowed_modes"), behavior_.state().at("mode")))
                    continue;
            }

            if (payload.contains("run_id")) {
                auto target = store_.get("run", payload.at("run_id"));
                require(!target.is_null() && target.at("actor") == payload.at("actor") &&
                            target.at("conversation_key") == input.at("route").at("key"),
                    "STEER_SCOPE_DENIED");
                store_.atomic([&] {
                    (void)runs_.steer(target.at("id"), payload);
                    input["state"] = "consumed";
                    input["run_id"] = target.at("id");
                    writer_.put("input", input.at("id"), input);
                    writer_.event("input.consumed", input, target.at("id"));
                });
                continue;
            }

            auto grants = policy_.current();
            if (!activity.empty()) {
                const auto definition = store_.get("activity", activity).at("definition");
                Json actions = Json::array();
                for (const auto& action : definition.at("actions"))
                    if (includes(grants.at("actions"), action))
                        actions.push_back(action);
                grants["actions"] = actions;
            }

            store_.atomic([&] {
                const auto id = runs_.create(input, grants);
                input["state"] = "consumed";
                input["run_id"] = id;
                writer_.put("input", input.at("id"), input);
                writer_.event("input.consumed", input, id);
            });
        } catch (const std::exception& error) {
            input["state"] = "rejected";
            input["error"] = failure(error);
            writer_.save("input", input.at("id"), input, "input.rejected", input);
        }
    }
}
}
