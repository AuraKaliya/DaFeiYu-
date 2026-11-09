#include "inbox.hpp"
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
    PluginRuntime plugins;
    CallRouter calls;
    PolicyGate policy;
    BehaviorController behavior;
    OperationCoordinator operations;
    ContextAssembler context;
    RunExecutor runs;
    InboxRouter inbox;
    Scheduler scheduler;
    Engine(const Profile& profile, const Path& path, PluginRuntime::Receiver receive)
        : profile_(profile), store(path), plugins(profile, path, store.instance_id(), std::move(receive)), calls(plugins, store),
          policy(store, profile), behavior(store), operations(store, plugins, calls, policy, profile),
          context(store, plugins, calls, policy, profile), runs(store, plugins, calls, policy, operations, context, profile),
          inbox(store, plugins, policy, behavior, runs, profile), scheduler(store, plugins, calls, behavior, inbox, runs, profile) {}
    void start() {
        plugins.start(); (void)plugins.snapshot(profile_.data.at("allowed_actions")); scheduler.start(); operations.recover(); runs.recover();
        store.atomic([&] { store.event("instance.started", {{"instance_id", store.instance_id()}}); });
    }
    Json command(const std::string& name, const Json& args) {
        if (name == "health") {
            const auto catalog = plugins.catalog(); bool degraded = false;
            for (const auto& plugin : catalog) if (plugin.at("status") != "ready") degraded = true;
            return {{"instance_id", store.instance_id()}, {"state", "running"}, {"health", degraded ? "degraded" : "ready"}, {"behavior", behavior.state()},
                {"plugins", catalog}, {"calls", calls.active()}, {"policy", policy.current()}};
        }
        if (name == "plugins") return {{"plugins", plugins.catalog()}, {"actions", plugins.actions()}};
        if (name == "input.accept") return inbox.accept("local", args.at("key"), {{"text", args.at("text")}, {"actor", profile_.data.at("owner")},
            {"conversation", args.value("conversation", std::string("local"))}, {"delivery_policy", "reply"}});
        if (name == "input.get") return store.get("input", args.at("id"));
        if (name == "run.get") return store.get("run", args.at("id"));
        if (name == "run.list") return store.all("run");
        if (name == "run.cancel") return runs.cancel(args.at("id"));
        if (name == "run.resume") return runs.resume(args.at("id"));
        if (name == "run.steer") {
            auto run = store.get("run", args.at("id")); require(!run.is_null(), "RUN_MISSING");
            return inbox.accept("local", args.at("key"), {{"run_id", run.at("id")}, {"text", args.at("text")}, {"actor", profile_.data.at("owner")}, {"conversation", run.at("conversation")}});
        }
        if (name == "behavior.set") return behavior.transition(args);
        if (name == "policy.set") return policy.revise(args);
        if (name == "activity.list") return store.all("activity");
        if (name == "activity.set") return behavior.activity(args.at("id"), args.at("enabled"), args.at("expected_revision"));
        if (name == "operation.list") return store.all("operation");
        if (name == "operation.query") { operations.query(args.at("id")); return {{"accepted", true}}; }
        if (name == "operation.reconcile") return operations.reconcile(args);
        if (name == "job.list") return store.all("job");
        if (name == "job.cancel") {
            auto job = store.get("job", args.at("id")); require(!job.is_null(), "JOB_MISSING");
            operations.cancel_jobs(job.at("run_id")); return {{"accepted", true}};
        }
        if (name == "events") return store.events(args.value("after", uint64_t{0}), args.value("limit", size_t{100}), args.value("run_id", std::string{}), args.value("types", Json::array()));
        if (name == "host.shutdown") return {{"accepted", true}};
        throw std::runtime_error("COMMAND_UNKNOWN");
    }
    Json execute(const std::string& name, Json args) {
        const auto key = args.value("_command_id", identifier()); args.erase("_command_id");
        static const std::set<std::string> mutations{"input.accept", "run.cancel", "run.resume", "run.steer", "behavior.set", "policy.set", "activity.set", "operation.reconcile", "job.cancel"};
        if (!mutations.contains(name)) return command(name, args);
        auto previous = store.get("command", key);
        if (!previous.is_null()) {
            require(previous.at("name") == name && previous.at("arguments") == args, "COMMAND_ID_CONFLICT");
            return previous.at("result");
        }
        Json result;
        store.atomic([&] { result = command(name, args); store.put("command", key, {{"id", key}, {"name", name}, {"arguments", args}, {"result", result}}); });
        return result;
    }
    void message(const PluginMessage& message) {
        const auto& body = message.body;
        const auto type = body.at("type").get<std::string>();
        try {
            if (type == "complete") { calls.complete(message); return; }
            if (type == "request_service") { operations.nested(message); return; }
            if (type == "register_job") { operations.register_job(message); return; }
            if (type == "job_result") { operations.job_result(message); return; }
            if (type == "progress") return;
            if (type == "read_state" || type == "submit_command" || type == "query_command") {
                const auto parent = calls.owned(message.plugin, body.at("call_id"));
                const auto key = body.at("key").get<std::string>(); require(!key.empty() && key.size() <= 100, "COMMAND_KEY_INVALID");
                Json result;
                if (type == "read_state") {
                    const auto object = body.at("object").get<std::string>();
                    require(object == "run" || object == "behavior", "STATE_SCOPE_DENIED");
                    result = object == "run" ? store.get("run", parent.scope.run) : behavior.state();
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
            if (type == "plugin_error") { store.atomic([&] { store.event("plugin.error", body.at("error"), {}, message.plugin); }); return; }
            const auto selection = plugins.selection(message.plugin);
            if (type == "post_input") {
                require(selection.value("accept_inputs", false), "INPUT_SOURCE_DENIED");
                const auto input = body.at("input");
                const auto principal = input.at("actor").get<std::string>();
                require(selection.at("principals").contains(principal), "INPUT_PRINCIPAL_DENIED");
                auto normalized = input; normalized["actor"] = selection.at("principals").at(principal);
                normalized.erase("activity"); normalized.erase("auto_depth"); normalized["delivery_policy"] = "reply";
                const auto receipt = inbox.accept(message.plugin, body.at("key"), normalized);
                plugins.send(message.plugin, {{"type", "input_receipt"}, {"key", body.at("key")}, {"result", receipt}});
                return;
            }
            const auto subscription_id = message.plugin + ":" + body.at("key").get<std::string>();
            if (type == "subscribe") {
                require(body.at("types").is_array() && !body.at("types").empty(), "SUBSCRIPTION_FILTER_REQUIRED");
                for (const auto& event : body.at("types")) require(includes(selection.value("observe", Json::array()), event), "SUBSCRIPTION_DENIED");
                auto subscription = store.get("subscription", subscription_id);
                if (subscription.is_null()) {
                    subscription = {{"id", subscription_id}, {"plugin", message.plugin}, {"key", body.at("key")}, {"types", body.at("types")},
                        {"cursor", 0}, {"sent", 0}, {"pending", nullptr}};
                    store.save("subscription", subscription_id, subscription, "subscription.created");
                } else require(subscription.at("types") == body.at("types"), "SUBSCRIPTION_CHANGED");
                return;
            }
            if (type == "ack_facts") {
                auto subscription = store.get("subscription", subscription_id);
                require(!subscription.is_null() && subscription.at("plugin") == message.plugin, "SUBSCRIPTION_OWNER_DENIED");
                require(body.at("sequence") == subscription.at("sent"), "SUBSCRIPTION_ACK_GAP");
                subscription["cursor"] = body.at("sequence"); subscription["pending"] = nullptr;
                store.atomic([&] { store.put("subscription", subscription_id, subscription); }); return;
            }
            throw std::runtime_error("HOST_MESSAGE_UNKNOWN");
        } catch (const std::exception& error) {
            store.atomic([&] { store.event("plugin.request_rejected", {{"request", body}, {"error", failure(error)}}, {}, message.plugin); });
            if (body.contains("key")) {
                try { plugins.send(message.plugin, {{"type", "reply"}, {"key", body.at("key")}, {"call_id", body.value("call_id", uint64_t{0})}, {"result", nullptr}, {"error", failure(error)}}); }
                catch (const std::exception&) { /* Failure remains observable in the core event log. */ }
            }
        }
    }
    void subscriptions() {
        for (auto subscription : store.all("subscription")) {
            try {
                const auto selection = plugins.selection(subscription.at("plugin"));
                bool authorized = true;
                for (const auto& type : subscription.at("types")) if (!includes(selection.value("observe", Json::array()), type)) authorized = false;
                if (!authorized) continue;
                if (!subscription.at("pending").is_null()) continue;
                auto page = store.events(subscription.at("cursor"), 32, {}, subscription.at("types"));
                if (page.at("cursor") == subscription.at("cursor")) continue;
                if (page.at("events").empty()) { subscription["cursor"] = page.at("cursor"); store.atomic([&] { store.put("subscription", subscription.at("id"), subscription); }); continue; }
                subscription["sent"] = page.at("cursor"); subscription["pending"] = page;
                store.atomic([&] { store.put("subscription", subscription.at("id"), subscription); });
                plugins.send(subscription.at("plugin"), {{"type", "facts"}, {"key", subscription.at("key")}, {"page", page}});
            } catch (const std::exception& error) { store.atomic([&] { store.event("subscription.unavailable", failure(error)); }); }
        }
    }
    void tick() { calls.tick(); scheduler.tick(); subscriptions(); }
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
            if (commands_allowed) requests.swap(commands);
            incoming.swap(messages);
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
            // Unacknowledged fact pages are replayed after restart.
            for (auto sub : engine.store.all("subscription")) { sub["pending"] = nullptr; engine.store.atomic([&] { engine.store.put("subscription", sub.at("id"), sub); }); }
            ready.set_value(); announced = true;
            for (;;) {
                receive(engine, true);
                { std::lock_guard lock(mutex); if (closing) break; }
                engine.tick();
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(10), [&] { return closing || !commands.empty() || !messages.empty() || !terminals.empty(); });
            }
            engine.store.atomic([&] { engine.store.event("instance.draining", Json::object()); });
            for (const auto& run : engine.store.all("run")) engine.operations.cancel_jobs(run.at("id"));
            engine.calls.cancel_all(); engine.plugins.quiesce();
            const auto deadline = SteadyClock::now() + std::chrono::milliseconds(profile.data.at("limits").at("shutdown_ms").get<int>());
            while (engine.calls.active() && SteadyClock::now() < deadline) {
                receive(engine, true);
                std::unique_lock lock(mutex); changed.wait_for(lock, std::chrono::milliseconds(10));
            }
            engine.plugins.stop(); // A native module is never unloaded while its code still executes.
            receive(engine, false);
            engine.store.atomic([&] { engine.store.event("instance.stopped", Json::object()); });
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
