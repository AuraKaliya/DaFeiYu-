#include "inbox.hpp"
#include "run_view.hpp"
#include "scopes.hpp"
#include "subscriptions.hpp"
#include "commands.hpp"
#include <dafeiyu/abi.h>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace dfy {
namespace {
class Engine {
    const Profile& profile_;
public:
    StateStore store;
    StateWriter diagnostics;
    PluginRuntime plugins;
    CallRouter calls;
    PolicyGate policy;
    BehaviorController behavior;
    Conversations conversations;
    ScopeManager scopes;
    OperationCoordinator operations;
    JobCoordinator jobs;
    ContextAssembler context;
    RunExecutor runs;
    InboxRouter inbox;
    Scheduler scheduler;
    Subscriptions subscriptions;
    CommandLedger commands;
    Engine(const Profile& profile, const Path& path, PluginRuntime::Receiver receive)
        : profile_(profile), store(path), diagnostics(store.claim("instance", {})), plugins(profile, path, store.instance_id(), std::move(receive)), calls(plugins, store),
          policy(store, profile), behavior(store), conversations(store, plugins, profile), scopes(store, plugins, calls, policy, conversations, profile), operations(store, plugins, calls, policy, conversations, scopes, profile),
          jobs(store, plugins, calls, operations, profile),
          context(store, plugins, calls, policy, scopes, profile), runs(store, plugins, calls, policy, operations, jobs, context, profile),
          inbox(store, plugins, policy, behavior, runs, conversations, profile), scheduler(store, plugins, calls, behavior, inbox, runs, profile), subscriptions(store, scopes, plugins), commands(store) {
        calls.scope_valid = [this](const CallScope& call) { return (call.scope_id.empty() || scopes.valid(call.scope_id)) && (call.registration_id.empty() || scopes.registration_active(call.registration_id)); };
        scopes.released = [this](const Json& registration) { subscriptions.release(registration); };
    }
    void start() {
        plugins.start(); conversations.start(); (void)plugins.snapshot(profile_.data.at("allowed_actions")); scheduler.start(); scopes.restore(); subscriptions.restore(); operations.recover(); jobs.recover(); runs.recover();
        store.atomic([&] { diagnostics.event("instance.started", {{"instance_id", store.instance_id()}}); });
    }
    Json command(const std::string& name, const Json& args) {
        if (name == "health") {
            const auto catalog = plugins.catalog(); bool degraded = false;
            for (const auto& plugin : catalog) if (plugin.at("status") != "ready") degraded = true;
            return {{"instance_id", store.instance_id()}, {"state", "running"}, {"health", degraded ? "degraded" : "ready"}, {"behavior", behavior.state()},
                {"plugins", catalog}, {"calls", calls.active()}, {"policy", policy.current()}, {"scopes", scopes.metrics()}, {"queries", store.metrics()}};
        }
        if (name == "plugins") return {{"plugins", plugins.catalog()}, {"actions", plugins.actions()}};
        if (name == "input.accept") return inbox.accept_local(args.at("key"), {{"text", args.at("text")},
            {"conversation", args.value("conversation", std::string("local"))}}, args.value("channel", std::string{}));
        if (name == "input.get") return store.get("input", args.at("id"));
        if (name == "run.get") return store.get("run", args.at("id"));
        if (name == "run.list") return store.page("run", args.value("cursor", uint64_t{0}), args.value("limit", size_t{100}));
        if (name == "run.view") return runs.view(args.at("id"));
        if (name == "conversation.close") { runs.cancel_conversation(args.at("id")); const auto result = conversations.close(args.at("id")); scopes.refresh(); return result; }
        if (name == "run.cancel") return runs.cancel(args.at("id"));
        if (name == "run.resume") return runs.resume(args.at("id"));
        if (name == "run.steer") {
            auto run = store.get("run", args.at("id")); require(!run.is_null(), "RUN_MISSING");
            return inbox.steer_local(args.at("key"), run, args.at("text"));
        }
        if (name == "behavior.set") return behavior.transition(args);
        if (name == "policy.set") return policy.revise(args);
        if (name == "activity.list") return scheduler.activity_views();
        if (name == "activity.set") return behavior.activity(args.at("id"), args.at("enabled"), args.at("expected_revision"));
        if (name == "operation.list") return store.page("operation", args.value("cursor", uint64_t{0}), args.value("limit", size_t{100}));
        if (name == "operation.query") { operations.query(args.at("id")); return {{"accepted", true}}; }
        if (name == "operation.reconcile") return operations.reconcile(args);
        if (name == "job.list") return store.page("job", args.value("cursor", uint64_t{0}), args.value("limit", size_t{100}));
        if (name == "job.cancel") {
            auto job = store.get("job", args.at("id")); require(!job.is_null(), "JOB_MISSING");
            jobs.cancel_jobs(job.at("run_id")); return {{"accepted", true}};
        }
        if (name == "events") return store.events(args.value("after", uint64_t{0}), args.value("limit", size_t{100}), args.value("run_id", std::string{}), args.value("types", Json::array()));
        if (name == "host.shutdown") return {{"accepted", true}};
        throw std::runtime_error("COMMAND_UNKNOWN");
    }
    Json execute(const std::string& name, Json args) {
        static const std::set<std::string> mutations{"input.accept", "run.cancel", "run.resume", "run.steer", "behavior.set", "policy.set", "activity.set", "operation.reconcile", "job.cancel", "conversation.close"};
        if (!mutations.contains(name)) return command(name, args);
        return commands.execute(name, std::move(args), [&](const Json& arguments) { return command(name, arguments); });
    }
    void message(const PluginMessage& message) {
        const auto& body = message.body;
        const auto type = body.at("type").get<std::string>();
        try {
            if (type == "complete") { calls.complete(message); return; }
            if (type == "request_service") { operations.nested(message); return; }
            if (type == "register_job") { jobs.register_job(message); return; }
            if (type == "job_result") { jobs.job_result(message); return; }
            if (type == "progress") return;
            if (type == "request_scoped") { scopes.request(message); return; }
            if (type == "ack_facts") { subscriptions.acknowledge(message.plugin, body); return; }
            if (type == "open_scope" || type == "close_scope" || type == "register_scoped" || type == "release_registration" || type == "subscribe" || type == "read_scoped") {
                Json result;
                if (type == "open_scope") result = scopes.open(message.plugin, body);
                else if (type == "close_scope") result = scopes.close(message.plugin, body.at("scope_id"));
                else if (type == "register_scoped") result = scopes.register_resource(message.plugin, body.at("scope_id"), body.at("key"), body.at("kind"), body.at("value"));
                else if (type == "release_registration") result = scopes.release(message.plugin, body.at("registration_id"));
                else if (type == "subscribe") result = subscriptions.subscribe(message.plugin, body);
                else result = scopes.read_state(message.plugin, body.at("scope_id"), body.at("object"));
                plugins.send(message.plugin, {{"type", "scope_reply"}, {"key", body.at("key")}, {"result", result}, {"error", nullptr}});
                return;
            }
            if (type == "read_state" || type == "submit_command" || type == "query_command") {
                const auto parent = calls.owned(message.plugin, body.at("call_id"));
                const auto key = body.at("key").get<std::string>(); require(!key.empty() && key.size() <= 100, "COMMAND_KEY_INVALID");
                Json result;
                if (type == "read_state") {
                    const auto object = body.at("object").get<std::string>();
                    require(object == "run" || object == "behavior", "STATE_SCOPE_DENIED");
                    result = object == "run" ? make_run_view(store.get("run", parent.scope.run), store.get("call_budget", "run:" + parent.scope.run)).to_json() : behavior.state();
                } else {
                    require(!parent.scope.operation.empty(), "COMMAND_OPERATION_REQUIRED");
                    const auto receipt_id = parent.scope.operation + ":command:" + key;
                    if (type == "query_command") result = store.get("command", receipt_id);
                    else {
                        const auto name = body.at("command").get<std::string>();
                        require(name == "behavior.set" || name == "activity.set", "PLUGIN_COMMAND_DENIED");
                        require(includes(plugins.selection(message.plugin).value("commands", Json::array()), name), "PLUGIN_COMMAND_DENIED");
                        policy.method(parent.scope, {{"effects", "local-write"}, {"resource", "instance"}});
                        auto args = body.at("arguments"); args["_command_id"] = receipt_id;
                        result = execute(name, args);
                    }
                }
                plugins.send(message.plugin, {{"type", "reply"}, {"call_id", parent.id}, {"key", key}, {"result", result}, {"error", nullptr}});
                return;
            }
            if (type == "plugin_error") { store.atomic([&] { diagnostics.event("plugin.error", body.at("error"), {}, message.plugin); }); return; }
            if (type == "post_input") {
                const auto receipt = inbox.accept_channel(message.plugin, body.at("channel"), body.at("key"), body.at("input"));
                plugins.send(message.plugin, {{"type", "input_receipt"}, {"key", body.at("key")}, {"result", receipt}});
                return;
            }
            throw std::runtime_error("HOST_MESSAGE_UNKNOWN");
        } catch (const std::exception& error) {
            store.atomic([&] { diagnostics.event("plugin.request_rejected", {{"request", body}, {"error", failure(error)}}, {}, message.plugin); });
            if (body.contains("key")) {
                const auto reply_type = type == "request_scoped" ? "scoped_reply" :
                    type == "open_scope" || type == "close_scope" || type == "register_scoped" || type == "release_registration" || type == "subscribe" || type == "read_scoped" ? "scope_reply" : "reply";
                try { plugins.send(message.plugin, {{"type", reply_type}, {"key", body.at("key")}, {"call_id", body.value("call_id", uint64_t{0})},
                    {"scope_id", body.value("scope_id", std::string{})}, {"result", nullptr}, {"error", failure(error)}}); }
                catch (const std::exception&) { /* Failure remains observable in the core event log. */ }
            }
        }
    }
    void tick() {
        calls.tick();
        for (const auto& change : store.take_changes()) if (change.kind == "operation") runs.operation_changed(change.id);
        for (const auto& operation : store.unobserved_operations()) runs.operation_changed(operation.at("id"));
        scopes.refresh(); scheduler.tick(); scopes.refresh(); subscriptions.tick();
    }
    SteadyClock::time_point wakeup() {
        if (store.changed()) return SteadyClock::now();
        const auto due = scheduler.next_wakeup();
        auto deadline = SteadyClock::now() + std::chrono::milliseconds(profile_.data.at("limits").at("reconcile_ms").get<int>());
        if (due > now_ms()) deadline = std::min(deadline, SteadyClock::now() + std::chrono::milliseconds(due - now_ms()));
        return std::min(deadline, calls.next_deadline());
    }

};
struct Command {
    std::string name;
    Json arguments;
    std::promise<Json> result;
};
}
struct InstanceRuntime::Impl {
    Profile profile;
    Path path;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Command>> commands;
    std::deque<PluginMessage> messages;
    std::map<std::string, PluginMessage> terminals;
    std::thread thread;
    bool closing{}, stopped{};
    std::promise<void> ready;
    Impl(Profile value, Path directory) : profile(std::move(value)), path(std::move(directory)) {}
    int post(PluginMessage message) {
        std::lock_guard lock(mutex);
        if (stopped) return DFY_CLOSED;
        const auto type = message.body.at("type").get<std::string>();
        if (type == "complete" || type == "job_result") {
            const auto key = message.plugin + ":" + type + ":" + (type == "complete" ? message.body.at("call_id").dump() : message.body.at("job_id").dump());
            if (terminals.contains(key)) return terminals.at(key).body == message.body ? DFY_OK : DFY_ERROR;
            terminals.emplace(key, std::move(message));
        } else {
            if (messages.size() >= 1024) return DFY_BUSY;
            messages.push_back(std::move(message));
        }
        changed.notify_one(); return DFY_OK;
    }
    void receive(Engine& engine, bool commands_allowed) {
        std::deque<std::shared_ptr<Command>> requests;
        std::deque<PluginMessage> incoming;
        {
            std::lock_guard lock(mutex);
            if (commands_allowed) for (size_t count = 0; !commands.empty() && count < 64; ++count) { requests.push_back(std::move(commands.front())); commands.pop_front(); }
            for (size_t count = 0; !messages.empty() && count < 64; ++count) { incoming.push_back(std::move(messages.front())); messages.pop_front(); }
            for (auto& [key, value] : terminals) { (void)key; incoming.push_back(std::move(value)); }
            terminals.clear();
        }
        for (auto& request : requests) {
            try {
                const auto result = engine.execute(request->name, request->arguments);
                request->result.set_value(result);
                if (request->name == "host.shutdown") { std::lock_guard lock(mutex); closing = true; }
            } catch (...) { request->result.set_exception(std::current_exception()); }
        }
        for (const auto& message : incoming) engine.message(message);
    }
    void run() {
        bool announced = false;
        try {
            Engine engine(profile, path, [this](PluginMessage value) { return post(std::move(value)); });
            engine.start();
            ready.set_value(); announced = true;
            for (;;) {
                receive(engine, true);
                { std::lock_guard lock(mutex); if (closing) break; }
                engine.tick();
                std::unique_lock lock(mutex);
                changed.wait_until(lock, engine.wakeup(), [&] { return closing || !commands.empty() || !messages.empty() || !terminals.empty(); });
            }
            engine.store.atomic([&] { engine.diagnostics.event("instance.draining", Json::object()); });
            engine.scopes.shutdown();
            for (const auto& run : engine.store.active_runs()) engine.jobs.cancel_jobs(run.at("id"));
            engine.calls.cancel_all(); engine.plugins.quiesce();
            const auto deadline = SteadyClock::now() + std::chrono::milliseconds(profile.data.at("limits").at("shutdown_ms").get<int>());
            while (engine.calls.active() && SteadyClock::now() < deadline) {
                receive(engine, true);
                std::unique_lock lock(mutex); changed.wait_for(lock, std::chrono::milliseconds(10));
            }
            engine.plugins.stop(); // A native module is never unloaded while its code still executes.
            receive(engine, false);
            engine.store.atomic([&] { engine.diagnostics.event("instance.stopped", Json::object()); });
        } catch (...) {
            if (!announced) ready.set_exception(std::current_exception());
            std::lock_guard lock(mutex);
            for (auto& command : commands) command->result.set_exception(std::current_exception());
            commands.clear();
        }
        std::lock_guard lock(mutex); stopped = true;
        for (auto& command : commands) command->result.set_exception(std::make_exception_ptr(std::runtime_error("INSTANCE_STOPPED")));
        commands.clear();
    }
};
InstanceRuntime::InstanceRuntime(Profile profile, Path state) : impl_(std::make_unique<Impl>(std::move(profile), std::move(state))) {
    auto ready = impl_->ready.get_future();
    impl_->thread = std::thread([this] { impl_->run(); });
    try { ready.get(); } catch (...) { impl_->thread.join(); throw; }
}
InstanceRuntime::~InstanceRuntime() { shutdown(); }
Json InstanceRuntime::request(const std::string& command, const Json& arguments) {
    auto request = std::make_shared<Command>(); request->name = command; request->arguments = arguments;
    auto result = request->result.get_future();
    { std::lock_guard lock(impl_->mutex); require(!impl_->closing && !impl_->stopped, "INSTANCE_STOPPED"); require(impl_->commands.size() < 256, "COMMAND_QUEUE_FULL"); impl_->commands.push_back(request); }
    impl_->changed.notify_one(); return result.get();
}
void InstanceRuntime::shutdown() {
    if (!impl_ || !impl_->thread.joinable()) return;
    { std::lock_guard lock(impl_->mutex); impl_->closing = true; }
    impl_->changed.notify_one(); impl_->thread.join();
}
}
