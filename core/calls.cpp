#include "calls.hpp"
namespace dfy {
Json CallScope::context() const {
    return {{"run_id", run}, {"operation_id", operation}, {"intent_revision", intent}, {"role", role}};
}
uint64_t CallRouter::request(const std::string& service, const std::string& method, const Json& input, CallScope scope,
    int timeout_ms, std::function<void(const Completion&)> done) {
    require(pending_.size() < 256, "CALL_LIMIT");
    const auto definition = plugins_.method(service, method);
    sdk::validate_value(definition.at("input"), input);
    const uint64_t ticket = next_++;
    const auto deadline = SteadyClock::now() + std::chrono::milliseconds(timeout_ms);
    pending_.emplace(ticket, PendingCall{ticket, plugins_.owner(service), service, method, scope, deadline, false, std::move(done)});
    try { plugins_.invoke(ticket, service, method, input, scope.context(), deadline); }
    catch (...) { pending_.erase(ticket); plugins_.release(ticket); throw; }
    return ticket;
}
const PendingCall& CallRouter::owned(const std::string& plugin, uint64_t ticket) const {
    const auto found = pending_.find(ticket);
    require(found != pending_.end() && found->second.plugin == plugin && !found->second.logical_done, "CALL_SCOPE_DENIED");
    return found->second;
}
void CallRouter::complete(const PluginMessage& message) {
    const auto ticket = message.body.at("call_id").get<uint64_t>();
    auto found = pending_.find(ticket);
    if (found == pending_.end()) return;
    require(found->second.plugin == message.plugin, "CALL_OWNER_DENIED");
    auto pending = std::move(found->second); pending_.erase(found); plugins_.release(ticket);
    Completion completion{message.body.value("result", Json{}), message.body.value("error", Json{}), pending.logical_done, true};
    if (completion.error.is_null()) {
        try { sdk::validate_value(plugins_.method(pending.service, pending.method).at("output"), completion.result); }
        catch (const std::exception& error) { completion.error = failure(error); completion.error["effects"] = pending.scope.effects == "read" ? "none" : "unknown"; }
    }
    store_.atomic([&] { store_.event(completion.late ? "call.late" : "call.completed",
        {{"call_id", ticket}, {"service", pending.service}, {"method", pending.method}, {"result", completion.result}, {"error", completion.error}}, pending.scope.run, message.plugin); });
    pending.done(completion);
}
void CallRouter::tick() {
    std::vector<uint64_t> expired;
    for (const auto& [id, pending] : pending_) if (!pending.logical_done && SteadyClock::now() >= pending.deadline) expired.push_back(id);
    for (const auto id : expired) {
        auto& pending = pending_.at(id); pending.logical_done = true; plugins_.cancel(id);
        auto callback = pending.done;
        callback({{}, sdk::error("DEADLINE_EXCEEDED", "deadline", pending.scope.effects == "read" ? "none" : "unknown", true), false, false});
    }
}
void CallRouter::cancel_run(const std::string& run) {
    std::vector<uint64_t> targets;
    for (const auto& [id, call] : pending_) if (call.scope.run == run && !call.logical_done) targets.push_back(id);
    for (const auto id : targets) {
        auto& pending = pending_.at(id); pending.logical_done = true; plugins_.cancel(id);
        auto callback = pending.done;
        callback({{}, sdk::error("CANCELLED", "cancelled", pending.scope.effects == "read" ? "none" : "unknown"), false, false});
    }
}
void CallRouter::cancel_all() {
    std::set<std::string> runs;
    for (const auto& [id, call] : pending_) { (void)id; runs.insert(call.scope.run); }
    for (const auto& run : runs) cancel_run(run);
}
bool CallRouter::pending_for(const std::string& run) const {
    for (const auto& [id, call] : pending_) { (void)id; if (call.scope.run == run) return true; }
    return false;
}
}
