#include "scopes.hpp"
#include "capabilities.hpp"
#include "run_view.hpp"

namespace dfy {
namespace {
// 移除仅在本次进程有效的作用域和注册令牌，生成可持久化记录。
Json persistent(Json value) {
    value.erase("_token");
    value.erase("scope_token");
    return value;
}
}

// 取得插件获准申请的指定作用域权限。
Json ScopeManager::permissions(const std::string& plugin, const std::string& kind) const {
    const auto selected = plugins_.selection(plugin);
    require(
        selected.contains("scope_permissions") && selected.at("scope_permissions").contains(kind), "SCOPE_KIND_DENIED");
    auto permission = selected.at("scope_permissions").at(kind);
    require(permission.at("resources").is_array(), "SCOPE_PERMISSION_INVALID");
    return permission;
}

// 判断作用域是否仍符合配置、权限、代次和来源授权。
// 步骤：检查插件与绑定快照 → 复核权限 → 按作用域类型校验生命周期 → 校验来源交付授权。
bool ScopeManager::current(const Json& scope) {
    try {
        if (!plugins_.plugin_ready(scope.at("plugin")))
            return false;
        if (scope.at("configuration") != fingerprint(plugins_.selection(scope.at("plugin")).dump()))
            return false;
        if (!plugins_.matches(scope.at("bindings")))
            return false;

        const auto policy = policy_.current();
        if (!scope.value("root_action", std::string{}).empty() &&
            !includes(policy.at("actions"), scope.at("root_action")))
            return false;
        if (!includes(policy.at("effects"), "read"))
            return false;
        for (const auto& resource : scope.at("grants").at("resources"))
            if (!permits(policy.at("resources"), resource))
                return false;

        const auto kind = scope.at("kind").get<std::string>();
        if (kind == "run") {
            const auto run = store_.get("run", scope.at("subject"));
            if (run.is_null() || terminal(run) || run.at("intent_revision") != scope.at("generation"))
                return false;
        } else if (kind == "conversation") {
            const auto conversation = store_.get("conversation", scope.at("subject"));
            if (conversation.is_null() || conversation.at("state") != "open" ||
                conversation.at("generation") != scope.at("generation"))
                return false;
        } else if (kind == "call") {
            if (scope.at("activation") != epoch_ || !calls_.active(scope.at("parent_call")))
                return false;
        } else if (kind != "instance")
            return false;

        conversations_.authorize(scope.at("source"));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// 生成对插件公开的作用域令牌、主体及代次描述。
Json ScopeManager::descriptor(const Json& scope) const {
    return {{"id", scope.at("_token")}, {"kind", scope.at("kind")}, {"subject", scope.at("subject")},
        {"generation", scope.at("generation")}, {"key", scope.at("key")}};
}

// 为有效作用域生成本次进程令牌，并保存激活或恢复状态。
std::string ScopeManager::activate(Json scope, bool restored) {
    require(scopes_.size() < profile_.data.at("limits").at("max_scopes").get<size_t>(), "SCOPE_LIMIT");

    const auto token = identifier();
    scope["_token"] = token;
    scope["state"] = "active";
    scope["activation"] = epoch_;
    writer_.save("scope", scope.at("id"), persistent(scope), restored ? "scope.restored" : "scope.opened",
        persistent(scope), scope.value("run", std::string{}), scope.at("conversation"));

    scopes_.emplace(token, scope);
    if (restored)
        plugins_.send(scope.at("plugin"), {{"type", "scope_restored"}, {"result", descriptor(scope)}});
    return token;
}

// 恢复仍有效的持久化作用域及注册，并重新签发临时令牌。
// 步骤：筛选可恢复作用域 → 激活并建立新令牌映射 → 恢复关联注册或关闭失效项 → 通知插件。
void ScopeManager::restore() {
    std::map<std::string, std::string> handles;
    for (auto scope : store_.live("scope")) {
        if (scope.at("kind") == "call" || !current(scope)) {
            scope["state"] = "closed";
            writer_.save("scope", scope.at("id"), scope, "scope.restore_denied");
            continue;
        }
        handles.emplace(scope.at("id").get<std::string>(), activate(scope, true));
    }

    for (auto item : store_.live("registration")) {
        const auto found = handles.find(item.at("scope_key").get<std::string>());
        if (found == handles.end()) {
            item["state"] = "closed";
            writer_.save("registration", item.at("id"), item, "registration.restore_denied");
            if (released)
                released(item);
            continue;
        }
        require(registrations_.size() < profile_.data.at("limits").at("max_registrations").get<size_t>(),
            "REGISTRATION_LIMIT");
        item["scope_token"] = found->second;
        item["_token"] = identifier();
        item["state"] = "active";
        registrations_.emplace(item.at("_token").get<std::string>(), item);
        writer_.save("registration", item.at("id"), persistent(item), "registration.restored");
        plugins_.send(
            item.at("plugin"), {{"type", "registration_restored"}, {"key", item.at("key")},
                                   {"result", {{"id", item.at("_token")}, {"scope_id", item.at("scope_token")},
                                                  {"kind", item.at("kind")}, {"value", item.at("value")}}}});
    }
}

// 在配置授权和父调用边界内创建或复用作用域。
// 步骤：校验类型与插件权限 → 继承父调用身份及资源交集 → 构造稳定身份并检查有效性 → 复用或激活作用域。
Json ScopeManager::open(const std::string& plugin, const Json& request) {
    require(plugins_.plugin_ready(plugin), "SCOPE_PLUGIN_UNAVAILABLE");

    const auto kind = request.at("kind").get<std::string>(), key = request.at("key").get<std::string>();
    require(
        includes(Json::array({"instance", "conversation", "run", "call"}), kind) && !key.empty() && key.size() <= 100,
        "SCOPE_REQUEST_INVALID");
    const auto permission = permissions(plugin, kind), policy = policy_.current();

    Json scope{{"plugin", plugin}, {"kind", kind}, {"key", key}, {"subject", store_.instance_id()}, {"generation", 1},
        {"run", ""}, {"conversation", ""}, {"source", nullptr}, {"parent_call", 0}, {"activation", epoch_},
        {"bindings", plugins_.plugin_snapshot(plugin)},
        {"configuration", fingerprint(plugins_.selection(plugin).dump())}};
    auto resources = intersect_permissions(permission.at("resources"), policy.at("resources"));

    if (kind != "instance") {
        const auto parent = calls_.owned(plugin, request.at("call_id"));
        scope["root_action"] = parent.scope.root_action;
        resources = intersect_permissions(resources, parent.scope.delegation.at("resources"));
        require(includes(parent.scope.delegation.at("effects"), "read"), "SCOPE_READ_DENIED");
        require(!parent.scope.run.empty(), "SCOPE_RUN_REQUIRED");
        const auto run = store_.get("run", parent.scope.run);
        scope["conversation"] = run.at("conversation");
        scope["source"] = run.at("delivery_binding");

        if (kind == "conversation") {
            const auto conversation = store_.get("conversation", run.at("conversation"));
            require(conversation.at("state") == "open", "SCOPE_CONVERSATION_CLOSED");
            scope["subject"] = conversation.at("id");
            scope["generation"] = conversation.at("generation");
        } else {
            scope["run"] = parent.scope.run;
            scope["generation"] = run.at("intent_revision");
            scope["subject"] = kind == "call" ? epoch_ + ":" + std::to_string(parent.id) : parent.scope.run;
            if (kind == "call")
                scope["parent_call"] = parent.id;
            else
                require(!terminal(run), "SCOPE_RUN_CLOSED");
        }
    }

    require(includes(policy.at("effects"), "read") && !resources.empty(), "SCOPE_RESOURCE_DENIED");
    scope["grants"] = {{"actions", Json::array()}, {"effects", Json::array({"read"})}, {"resources", resources}};
    scope["id"] = Json::array({plugin, kind, scope.at("subject"), scope.at("generation"), key}).dump();

    for (const auto& [token, existing] : scopes_)
        if (existing.at("id") == scope.at("id") && current(existing)) {
            (void)token;
            return descriptor(existing);
        }
    require(current(scope), "SCOPE_AUTHORITY_DENIED");
    return descriptor(scopes_.at(activate(scope, false)));
}

// 取得属于指定插件且当前仍有效的作用域。
Json ScopeManager::owned(const std::string& plugin, const std::string& id) {
    const auto found = scopes_.find(id);
    require(found != scopes_.end() && found->second.at("plugin") == plugin, "SCOPE_OWNER_DENIED");
    require(current(found->second), "SCOPE_CLOSED");
    return found->second;
}

// 判断作用域令牌是否存在且仍然有效。
bool ScopeManager::valid(const std::string& id) {
    const auto found = scopes_.find(id);
    return found != scopes_.end() && current(found->second);
}

// 在作用域内幂等登记上下文、动作可见性或事实订阅。
// 步骤：校验作用域权限 → 核对资源类型和所有者 → 复用相同注册或检查容量 → 保存注册并返回令牌。
Json ScopeManager::register_resource(const std::string& plugin, const std::string& token, const std::string& key,
    const std::string& kind, const Json& value) {
    const auto scope = owned(plugin, token), permission = permissions(plugin, scope.at("kind"));
    require(!key.empty() && key.size() <= 100 && includes(permission.value("registrations", Json::array()), kind),
        "REGISTRATION_DENIED");

    if (kind == "context") {
        const auto service = value.at("service").get<std::string>();
        require(plugins_.owner(service) == plugin && plugins_.service(service).at("contract") == "dafeiyu.context",
            "CONTEXT_REGISTRATION_DENIED");
    } else if (kind == "action") {
        const auto action = plugins_.action(value.at("action"));
        require(action.at("plugin_id") == plugin && action.value("visibility", std::string("instance")) == "scoped",
            "ACTION_REGISTRATION_DENIED");
    } else
        require(kind == "subscription", "REGISTRATION_KIND_INVALID");

    const auto id = Json::array({scope.at("id"), key}).dump();
    for (const auto& [handle, item] : registrations_)
        if (item.at("id") == id) {
            require(item.at("kind") == kind && item.at("value") == value, "REGISTRATION_KEY_CONFLICT");
            return {{"id", handle}, {"persistent_id", id}, {"scope_id", token}, {"key", key}};
        }

    require(
        registrations_.size() < profile_.data.at("limits").at("max_registrations").get<size_t>(), "REGISTRATION_LIMIT");
    Json item{{"id", id}, {"plugin", plugin}, {"scope_key", scope.at("id")}, {"scope_token", token}, {"key", key},
        {"kind", kind}, {"value", value}, {"state", "active"}, {"_token", identifier()}};
    writer_.save("registration", id, persistent(item), "registration.created", persistent(item), scope.at("run"),
        scope.at("conversation"));
    registrations_.emplace(item.at("_token").get<std::string>(), item);
    return {{"id", item.at("_token")}, {"persistent_id", id}, {"scope_id", token}, {"key", key}};
}

// 释放插件所属的资源注册，并撤销依赖该注册的调用。
// 步骤：核对注册归属 → 移除活动令牌并保存关闭状态 → 取消相关调用并通知订阅清理。
Json ScopeManager::release(const std::string& plugin, const std::string& id) {
    const auto found = registrations_.find(id);
    if (found == registrations_.end())
        return {{"released", false}};
    auto item = found->second;
    require(item.at("plugin") == plugin, "REGISTRATION_OWNER_DENIED");

    registrations_.erase(found);
    item["state"] = "closed";
    const auto scope = store_.get("scope", item.at("scope_key"));
    writer_.save("registration", item.at("id"), persistent(item), "registration.released", Json::object(),
        scope.at("run"), scope.at("conversation"));

    calls_.cancel_registration(id);
    if (released)
        released(item);
    return {{"released", true}};
}

// 关闭或挂起作用域，并同步处理注册和在途调用。
// 步骤：移除活动作用域并保存状态 → 释放或挂起所属注册 → 取消作用域调用 → 通知插件生命周期结束。
void ScopeManager::close_impl(const std::string& id, const std::string& reason, bool suspend) {
    const auto found = scopes_.find(id);
    if (found == scopes_.end())
        return;
    auto scope = found->second;
    scopes_.erase(found);

    scope["state"] = suspend && scope.at("kind") != "call" ? "suspended" : "closed";
    writer_.save("scope", scope.at("id"), persistent(scope), "scope.closed", {{"reason", reason}}, scope.at("run"),
        scope.at("conversation"));

    std::vector<std::string> resources;
    for (const auto& [handle, item] : registrations_)
        if (item.at("scope_token") == id)
            resources.push_back(handle);
    for (const auto& handle : resources) {
        if (scope.at("state") == "suspended") {
            auto item = registrations_.at(handle);
            registrations_.erase(handle);
            item["state"] = "suspended";
            writer_.save("registration", item.at("id"), persistent(item), "registration.suspended");
            calls_.cancel_registration(handle);
            if (released)
                released(item);
        } else
            (void)release(scope.at("plugin"), handle);
    }

    calls_.cancel_scope(id);
    try {
        plugins_.send(scope.at("plugin"), {{"type", "scope_closed"}, {"scope_id", id}, {"key", scope.at("key")},
                                              {"reason", reason}, {"last_sequence", store_.sequence()}});
    } catch (const std::exception&) {
    }
}

// 核对所有者后显式关闭作用域，重复关闭返回未关闭标记。
Json ScopeManager::close(const std::string& plugin, const std::string& id) {
    const auto found = scopes_.find(id);
    if (found == scopes_.end())
        return {{"closed", false}};
    require(found->second.at("plugin") == plugin, "SCOPE_OWNER_DENIED");
    close_impl(id, "released", false);
    return {{"closed", true}};
}

// 查找失效作用域并统一关闭其关联资源。
void ScopeManager::refresh() {
    std::vector<std::string> expired;
    for (const auto& [id, scope] : scopes_)
        if (!current(scope))
            expired.push_back(id);

    for (const auto& id : expired)
        close_impl(id, "lifetime-ended", false);
}

// 在实例停止时关闭调用作用域，并挂起可恢复作用域。
void ScopeManager::shutdown() {
    std::vector<std::string> ids;
    for (const auto& [id, value] : scopes_) {
        (void)value;
        ids.push_back(id);
    }
    for (const auto& id : ids)
        close_impl(id, "instance-stopping", true);
}

// 读取活动资源注册，令牌缺失时返回空值。
Json ScopeManager::registration(const std::string& id) const {
    const auto found = registrations_.find(id);
    return found == registrations_.end() ? Json{} : found->second;
}

// 判断资源注册及其所属作用域是否均有效。
bool ScopeManager::registration_active(const std::string& id) {
    const auto item = registration(id);
    return !item.is_null() && valid(item.at("scope_token"));
}

// 根据实例、会话或任务边界判断作用域是否覆盖运行任务。
bool ScopeManager::contains(const Json& scope, const Json& run) const {
    if (scope.at("kind") == "instance")
        return true;
    if (scope.at("kind") == "conversation")
        return scope.at("subject") == run.at("conversation");
    return scope.at("run") == run.at("id");
}

// 为限定作用域的动作查找覆盖当前任务的有效注册。
std::string ScopeManager::action_registration(const Json& action, const Json& run) {
    if (action.value("visibility", std::string("instance")) != "scoped")
        return {};

    for (const auto& [token, item] : registrations_)
        if (item.at("kind") == "action" && item.at("value").at("action") == action.at("id") &&
            registration_active(token) && contains(scopes_.at(item.at("scope_token").get<std::string>()), run))
            return token;
    throw std::runtime_error("ACTION_SCOPE_UNAVAILABLE");
}

// 收集覆盖当前运行任务的有效上下文服务注册。
std::vector<std::pair<std::string, std::string>> ScopeManager::contexts(const Json& run) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& [token, item] : registrations_)
        if (item.at("kind") == "context" && registration_active(token) &&
            contains(scopes_.at(item.at("scope_token").get<std::string>()), run))
            result.emplace_back(item.at("value").at("service"), token);
    return result;
}

// 在作用域授权内提交只读服务调用，并限制回调结果的使用期限。
// 步骤：验证作用域、依赖与契约 → 构造受限调用身份及共享预算 → 检查权限和交付查询范围 → 派发并在回调中复核作用域。
void ScopeManager::request(const PluginMessage& message) {
    const auto& body = message.body;
    const auto scope = owned(message.plugin, body.at("scope_id"));

    require(!body.at("key").get<std::string>().empty() && body.at("key").get<std::string>().size() <= 100,
        "SCOPE_REQUEST_KEY_INVALID");
    const auto binding = body.at("binding").get<std::string>(), method = body.at("method").get<std::string>();
    require(plugins_.dependency(message.plugin, binding) && scope.at("bindings").at("bindings").contains(binding),
        "SCOPE_BINDING_DENIED");

    const auto service = scope.at("bindings").at("bindings").at(binding).get<std::string>();
    const auto contract = plugins_.service(service).at("contract").get<std::string>();
    require(contract != "dafeiyu.model" && contract != "dafeiyu.action-executor", "SCOPE_CONTROLLED_DISPATCH_REQUIRED");
    const auto definition = plugins_.method(service, method);
    require(definition.at("effects") == "read", "SCOPE_WRITE_REQUIRES_RUN_OPERATION");

    CallScope call;
    call.scope_id = body.at("scope_id");
    call.scoped_request = true;
    call.role = "scope." + scope.at("kind").get<std::string>();
    call.run = scope.at("run");
    call.conversation = scope.at("conversation");
    call.intent = scope.at("generation");
    call.bindings = scope.at("bindings").at("bindings");
    call.grants = scope.at("grants");
    call.delegation = {{"effects", Json::array({"read"})}, {"resources", call.grants.at("resources")},
        {"write_resources", Json::array()}};
    call.parent_call = scope.at("kind") == "call" ? scope.at("parent_call").get<uint64_t>() : 0;

    call.budget_id = call.run.empty() ? "scope:" + epoch_ + ":" + scope.at("id").get<std::string>() : "run:" + call.run;
    call.nested_limit = call.run.empty() ? profile_.data.at("limits").at("scope_calls").get<int>()
                                         : store_.get("run", call.run).at("limits").at("nested_calls").get<int>();

    policy_.method(call, definition);

    if (contract == "dafeiyu.channel") {
        const auto operation = store_.get("operation", body.at("input").at("id"));
        require(!operation.is_null() && operation.at("kind") == "delivery" &&
                    operation.at("descriptor").at("service") == service &&
                    contains(scope, store_.get("run", operation.at("run_id"))),
            "DELIVERY_QUERY_SCOPE_DENIED");
        conversations_.validate_identity(operation.at("input").at("binding"));
    }

    const auto requested = body.value("timeout_ms", profile_.data.at("limits").at("call_timeout_ms").get<int>());
    calls_.request(service, method, body.at("input"), call,
        std::min(requested, profile_.data.at("limits").at("call_timeout_ms").get<int>()),
        [this, plugin = message.plugin, key = body.at("key"), token = call.scope_id](const Completion& value) {
            if (value.late)
                return;

            const bool alive = valid(token);
            plugins_.send(plugin,
                {{"type", "scoped_reply"}, {"key", key}, {"scope_id", token}, {"result", alive ? value.result : Json{}},
                    {"error", alive ? value.error : sdk::error("SCOPE_CLOSED", "cancelled")},
                    {"terminal_only", !alive}});
        });
}

// 按作用域边界读取运行视图、会话或行为状态。
Json ScopeManager::read_state(const std::string& plugin, const std::string& token, const std::string& object) {
    const auto scope = owned(plugin, token);
    require(permits(scope.at("grants").at("resources"), "instance"), "SCOPE_RESOURCE_DENIED");
    if (object == "run") {
        require(!scope.at("run").get<std::string>().empty(), "SCOPE_RUN_REQUIRED");
        const auto run = scope.at("run").get<std::string>();
        return make_run_view(store_.get("run", run), store_.get("call_budget", "run:" + run)).to_json();
    }
    if (object == "conversation") {
        require(!scope.at("conversation").get<std::string>().empty(), "SCOPE_CONVERSATION_REQUIRED");
        return store_.get("conversation", scope.at("conversation"));
    }
    require(object == "behavior", "STATE_SCOPE_DENIED");
    return store_.get("system", "behavior");
}
}
