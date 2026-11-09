#include "calls.hpp"
#include "capabilities.hpp"
namespace dfy {
Json CallScope::context() const {
    return {{"run_id", run}, {"operation_id", operation}, {"intent_revision", intent}, {"role", role},
        {"parent_call_id", parent_call}, {"scope_id", scope_id}, {"registration_id", registration_id}, {"delegation", delegation}};
}
bool CallRouter::active(uint64_t ticket) const {
    const auto found = pending_.find(ticket);
    return found != pending_.end() && !found->second.logical_done && SteadyClock::now() < found->second.deadline && scope_valid(found->second.scope);
}
uint64_t CallRouter::request(const std::string& service, const std::string& method, const Json& input, CallScope scope,
    int timeout_ms, std::function<void(const Completion&)> done) {
    require(pending_.size() < 256, "CALL_LIMIT");
    require(timeout_ms > 0, "CALL_TIMEOUT_INVALID");
    const auto transport = plugins_.method(service, method);
    sdk::validate_value(transport.at("input"), input);
    require(scope_valid(scope), "SCOPE_CLOSED");
    const auto definition = scope.action_definition.is_null() ? transport : scope.action_definition;
    const uint64_t ticket = next_++;
    if (scope.timeout_ms > 0) timeout_ms = std::min(timeout_ms, scope.timeout_ms);
    auto deadline = SteadyClock::now() + std::chrono::milliseconds(timeout_ms);
    Json inherited{{"effects", scope.grants.value("effects", Json::array({"read"}))},
        {"resources", scope.grants.value("resources", Json::array({"instance"}))},
        {"write_resources", scope.grants.value("resources", Json::array())}};
    if (scope.parent_call) {
        require(active(scope.parent_call), "PARENT_CALL_CLOSED");
        const auto& parent = pending_.at(scope.parent_call);
        require(parent.scope.run == scope.run && parent.scope.intent == scope.intent, "CHILD_SCOPE_MISMATCH");
        deadline = std::min(deadline, parent.deadline);
        inherited = parent.scope.delegation;
        require(includes(inherited.at("effects"), definition.at("effects")), "NESTED_EFFECT_DENIED");
        for (const auto& resource : resources_of(definition)) require(permits(inherited.at("resources"), resource), "NESTED_RESOURCE_DENIED");
        if (definition.at("effects") != "read") for (const auto& resource : resources_of(definition))
            require(permits(inherited.at("write_resources"), resource), "NESTED_RESOURCE_DENIED");
        scope.budget_id = parent.scope.budget_id;
        scope.nested_limit = parent.scope.nested_limit;
    } else if (scope.budget_id.empty()) scope.budget_id = scope.run.empty() ? "call:" + epoch_ + ":" + std::to_string(ticket) : "run:" + scope.run;
    if (scope.parent_call || scope.scoped_request) {
        auto budget = store_.get("call_budget", scope.budget_id);
        if (budget.is_null()) budget = {{"used", 0}, {"limit", scope.nested_limit}};
        require(budget.at("used").get<int>() < budget.at("limit").get<int>(), "NESTED_CALL_BUDGET_EXHAUSTED");
        budget["used"] = budget.at("used").get<int>() + 1;
        // Accepted dispatch attempts consume the shared budget even if the provider rejects dispatch.
        writer_.save("call_budget", scope.budget_id, budget, "call.budget_reserved", budget, scope.run, scope.conversation);
    }
    const auto declared = delegation_of(definition);
    scope.delegation = {{"effects", intersect_permissions(inherited.at("effects"), declared.at("effects"))},
        {"resources", intersect_permissions(inherited.at("resources"), declared.at("resources"))},
        {"write_resources", definition.at("effects") == "read" ? Json::array() : intersect_permissions(inherited.at("write_resources"), resources_of(definition))}};
    scope.effects = definition.at("effects"); scope.resource = definition.at("resource");
    pending_.emplace(ticket, PendingCall{ticket, plugins_.owner(service), service, method, scope, deadline, false, std::move(done)});
    try {
        store_.atomic([&] { writer_.event("call.started", {{"epoch", epoch_}, {"call_id", ticket}, {"parent_call_id", scope.parent_call},
            {"service", service}, {"method", method}, {"delegation", scope.delegation}, {"budget_id", scope.budget_id},
            {"deadline_steady_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count()}}, scope.run, "core", {}, scope.conversation); });
        require(SteadyClock::now() < deadline, "CALL_DEADLINE_EXCEEDED");
        if (scope.parent_call) require(active(scope.parent_call), "PARENT_CALL_CLOSED");
        plugins_.invoke(ticket, service, method, input, scope.context(), deadline);
    } catch (...) { pending_.erase(ticket); plugins_.release(ticket); throw; }
    return ticket;
}
const PendingCall& CallRouter::owned(const std::string& plugin, uint64_t ticket) const {
    require(active(ticket) && pending_.at(ticket).plugin == plugin, "CALL_SCOPE_DENIED");
    return pending_.at(ticket);
}
void CallRouter::terminate(uint64_t root, const std::string& code) {
    std::set<uint64_t> subtree{root};
    bool added = true;
    while (added) {
        added = false;
        for (const auto& [id, call] : pending_) if (call.scope.parent_call && subtree.contains(call.scope.parent_call))
            added = subtree.insert(id).second || added;
    }
    std::vector<uint64_t> notify;
    for (const auto id : subtree) {
        auto found = pending_.find(id);
        if (found == pending_.end() || found->second.logical_done) continue;
        found->second.logical_done = true;
        plugins_.cancel(id);
        notify.push_back(id);
    }
    // All qualifications are revoked before callbacks can issue new work.
    for (auto it = notify.rbegin(); it != notify.rend(); ++it) {
        auto found = pending_.find(*it); if (found == pending_.end()) continue;
        const auto call = found->second;
        const auto reason = sdk::error(code, code == "DEADLINE_EXCEEDED" ? "deadline" : "cancelled",
            call.scope.effects == "read" ? "none" : "unknown", code == "DEADLINE_EXCEEDED");
        store_.atomic([&] { writer_.event("call.logical_ended", {{"epoch", epoch_}, {"call_id", call.id},
            {"parent_call_id", call.scope.parent_call}, {"error", reason}}, call.scope.run, "core", {}, call.scope.conversation); });
        call.done({{}, reason, false, false});
    }
}
void CallRouter::complete(const PluginMessage& message) {
    const auto ticket = message.body.at("call_id").get<uint64_t>();
    auto found = pending_.find(ticket);
    if (found == pending_.end()) return;
    require(found->second.plugin == message.plugin, "CALL_OWNER_DENIED");
    if (!found->second.logical_done && !scope_valid(found->second.scope)) terminate(ticket, "SCOPE_CLOSED");
    if (!found->second.logical_done && SteadyClock::now() >= found->second.deadline) terminate(ticket, "DEADLINE_EXCEEDED");
    found = pending_.find(ticket);
    auto pending = std::move(found->second); pending_.erase(found); plugins_.release(ticket);
    // Removing the physically completed parent suppresses replies to it, but children still settle.
    terminate(ticket, "PARENT_CALL_ENDED");
    Completion completion{message.body.value("result", Json{}), message.body.value("error", Json{}), pending.logical_done, true};
    if (completion.error.is_null()) {
        try { sdk::validate_value(plugins_.method(pending.service, pending.method).at("output"), completion.result); }
        catch (const std::exception& error) { completion.error = failure(error); completion.error["effects"] = pending.scope.effects == "read" ? "none" : "unknown"; }
    }
    store_.atomic([&] { writer_.event(completion.late ? "call.late" : "call.completed",
        {{"epoch", epoch_}, {"call_id", ticket}, {"parent_call_id", pending.scope.parent_call},
        {"service", pending.service}, {"method", pending.method}, {"result", completion.result}, {"error", completion.error}}, pending.scope.run, message.plugin, {}, pending.scope.conversation); });
    pending.done(completion);
}
void CallRouter::tick() {
    std::vector<uint64_t> expired;
    for (const auto& [id, pending] : pending_) if (!pending.logical_done && SteadyClock::now() >= pending.deadline) expired.push_back(id);
    for (const auto id : expired) terminate(id, "DEADLINE_EXCEEDED");
}
void CallRouter::cancel_run(const std::string& run) {
    std::vector<uint64_t> targets;
    for (const auto& [id, call] : pending_) if (call.scope.run == run && !call.logical_done) targets.push_back(id);
    for (const auto id : targets) terminate(id, "CANCELLED");
}
void CallRouter::cancel_all() {
    std::vector<uint64_t> targets;
    for (const auto& [id, call] : pending_) { (void)call; targets.push_back(id); }
    for (const auto id : targets) terminate(id, "CANCELLED");
}
void CallRouter::cancel_scope(const std::string& scope) {
    std::vector<uint64_t> targets;
    for (const auto& [id, call] : pending_) if (call.scope.scope_id == scope) targets.push_back(id);
    for (const auto id : targets) terminate(id, "SCOPE_CLOSED");
}
void CallRouter::cancel_registration(const std::string& registration) {
    std::vector<uint64_t> targets;
    for (const auto& [id, call] : pending_) if (call.scope.registration_id == registration) targets.push_back(id);
    for (const auto id : targets) terminate(id, "REGISTRATION_RELEASED");
}
SteadyClock::time_point CallRouter::next_deadline() const {
    auto deadline = SteadyClock::time_point::max();
    for (const auto& [id, call] : pending_) { (void)id; if (!call.logical_done) deadline = std::min(deadline, call.deadline); }
    return deadline;
}
bool CallRouter::pending_for(const std::string& run) const {
    for (const auto& [id, call] : pending_) { (void)id; if (call.scope.run == run) return true; }
    return false;
}
}
