#include "plugins.hpp"
#include <dafeiyu/plugin.hpp>
#include <dafeiyu/contracts.hpp>
#include "capabilities.hpp"
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dfy {
namespace {
bool valid_name(const std::string& value) {
    if (value.empty() || value.size() > 120 || value.front() < 'a' || value.front() > 'z') return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-'; });
}
class Worker {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> work_;
    bool closing_{};
    std::thread thread_;
public:
    Worker() : thread_([this] {
        for (;;) {
            std::function<void()> task;
            { std::unique_lock lock(mutex_); changed_.wait(lock, [&] { return closing_ || !work_.empty(); });
              if (work_.empty() && closing_) return; task = std::move(work_.front()); work_.pop_front(); }
            task();
        }
    }) {}
    ~Worker() { { std::lock_guard lock(mutex_); closing_ = true; } changed_.notify_one(); thread_.join(); }
    void post(std::function<void()> task) {
        { std::lock_guard lock(mutex_); require(!closing_, "WORKER_CLOSED"); require(work_.size() < 1024, "WORKER_BUSY"); work_.push_back(std::move(task)); }
        changed_.notify_one();
    }
    void sync(std::function<void()> task) {
        auto job = std::make_shared<std::packaged_task<void()>>(std::move(task)); auto future = job->get_future();
        post([job] { (*job)(); }); future.get();
    }
};
struct Library {
#ifdef _WIN32
    HMODULE handle;
    explicit Library(const Path& path) : handle(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) { require(handle != nullptr, "PLUGIN_LOAD"); }
    ~Library() { FreeLibrary(handle); }
    dfy_query_fn query() const { return reinterpret_cast<dfy_query_fn>(GetProcAddress(handle, "dfy_plugin_query")); }
#else
    void* handle;
    explicit Library(const Path& path) : handle(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) { require(handle != nullptr, "PLUGIN_LOAD"); }
    ~Library() { dlclose(handle); }
    dfy_query_fn query() const { return reinterpret_cast<dfy_query_fn>(dlsym(handle, "dfy_plugin_query")); }
#endif
};
Path child(const Path& directory, const std::string& name) {
    const auto root = std::filesystem::canonical(directory);
    const auto path = std::filesystem::canonical(root / sdk::path_from_utf8(name));
    const auto relative = path.lexically_relative(root);
    require(!relative.empty() && !relative.is_absolute() && *relative.begin() != "..", "PLUGIN_PATH_ESCAPE");
    return path;
}
struct Errors {
    Json value;
    static int32_t DFY_CALL emit(void* raw, dfy_bytes text) noexcept {
        try { static_cast<Errors*>(raw)->value = Json::parse(sdk::text(text)); return DFY_OK; } catch (...) { return DFY_ERROR; }
    }
    dfy_sink sink() { return {this, emit}; }
};
struct Control { bool cancelled{}; SteadyClock::time_point deadline; };
struct Loaded {
    Json manifest, selected, action_definitions = Json::array();
    std::string id, data, instance, status{"discovered"}, diagnostic;
    std::unique_ptr<Library> library;
    Worker worker;
    dfy_plugin_api api{};
    dfy_host_api host{};
    void* object{};
    bool registering{true}, started{};
    std::set<std::string> registrations;
    std::mutex mutex;
    bool closed{};
    std::map<uint64_t, Control> calls;
    std::set<std::string> jobs;
    PluginRuntime::Receiver receive;
    static int32_t DFY_CALL registration(void* raw, dfy_bytes kind, dfy_bytes body) noexcept {
        auto& self = *static_cast<Loaded*>(raw);
        try {
            if (!self.registering) return DFY_CLOSED;
            auto value = Json::parse(sdk::text(body));
            if (sdk::text(kind) == "service") {
                const auto name = value.get<std::string>();
                for (const auto& service : self.manifest.at("services")) if (service.at("id") == name)
                    return self.registrations.insert(name).second ? DFY_OK : DFY_ERROR;
            } else if (sdk::text(kind) == "action") {
                const auto prefix = self.manifest.value("discovery_prefix", std::string{});
                require(!prefix.empty() && value.at("id").get<std::string>().starts_with(prefix), "DISCOVERY_SCOPE_DENIED");
                self.action_definitions.push_back(value); return DFY_OK;
            }
        } catch (...) { return DFY_ERROR; }
        return DFY_ERROR;
    }
    static int32_t DFY_CALL post(void* raw, dfy_bytes bytes) noexcept {
        auto& self = *static_cast<Loaded*>(raw);
        try {
            auto value = Json::parse(sdk::text(bytes));
            { std::lock_guard lock(self.mutex);
              if (self.closed) return DFY_CLOSED;
              const auto type = value.at("type").get<std::string>();
              if (type == "complete" || type == "progress" || type == "request_service" || type == "register_job" ||
                  type == "submit_command" || type == "query_command" || type == "read_state") {
                  if (!self.calls.contains(value.at("call_id").get<uint64_t>())) return DFY_CLOSED;
              }
              if (type == "job_result" && !self.jobs.contains(value.at("job_id").get<std::string>())) return DFY_ERROR;
            }
            return self.receive({self.id, std::move(value)});
        } catch (...) { return DFY_ERROR; }
    }
    static int32_t DFY_CALL cancelled(void* raw, uint64_t call) noexcept {
        auto& self = *static_cast<Loaded*>(raw);
        std::lock_guard lock(self.mutex);
        const auto found = self.calls.find(call);
        return found == self.calls.end() || found->second.cancelled || SteadyClock::now() >= found->second.deadline;
    }
    void close() {
        if (!object) return;
        worker.sync([&] { api.quiesce(object); api.stop(object); api.destroy(object); object = nullptr; });
        { std::lock_guard lock(mutex); closed = true; calls.clear(); jobs.clear(); }
        status = "stopped";
    }
    ~Loaded() { try { close(); } catch (...) {} }
};
void validate_method(const Json& method) {
    sdk::validate_schema(method.at("input")); sdk::validate_schema(method.at("output"));
    require(includes(Json::array({"read", "local-write", "external-write"}), method.at("effects")), "METHOD_EFFECT_INVALID");
    require(method.at("resource").is_string(), "METHOD_RESOURCE_INVALID");
    validate_permissions(method);
}
}
struct PluginRuntime::Impl {
    Profile profile;
    Path state;
    std::string instance;
    Receiver receive;
    std::map<std::string, std::unique_ptr<Loaded>> plugins;
    std::map<std::string, Json> services, action_map;
    std::vector<std::string> order;
    bool draining{};
    Impl(const Profile& p, Path s, std::string i, Receiver r) : profile(p), state(std::move(s)), instance(std::move(i)), receive(std::move(r)) {}
};
PluginRuntime::PluginRuntime(const Profile& profile, const Path& state, const std::string& instance, Receiver receive)
    : impl_(std::make_unique<Impl>(profile, state, instance, std::move(receive))) {}
PluginRuntime::~PluginRuntime() { stop(); }
void PluginRuntime::start() {
    std::map<std::string, Json> contracts;
    for (const auto& selected : impl_->profile.data.at("plugins")) {
        auto plugin = std::make_unique<Loaded>();
        plugin->manifest = read_json(child(impl_->profile.plugins, selected.at("manifest")));
        plugin->selected = selected; plugin->id = plugin->manifest.at("id");
        require(plugin->manifest.at("abi_version") == DFY_ABI_VERSION, "PLUGIN_ABI_UNSUPPORTED");
        require(valid_name(plugin->id), "PLUGIN_ID_INVALID");
        require(!impl_->plugins.contains(plugin->id), "PLUGIN_DUPLICATE");
        plugin->receive = impl_->receive; plugin->instance = impl_->instance;
        plugin->data = sdk::path_text(std::filesystem::absolute(impl_->state / "plugins" / sdk::path_from_utf8(plugin->id)));
        std::filesystem::create_directories(sdk::path_from_utf8(plugin->data));
        for (auto service : plugin->manifest.at("services")) {
            service = sdk::contracts::resolve_service(std::move(service));
            const auto id = service.at("id").get<std::string>();
            require(valid_name(id) && valid_name(service.at("contract")), "SERVICE_ID_INVALID");
            require(service.at("version").get<int>() > 0 && !service.at("methods").empty(), "CONTRACT_VERSION_INVALID");
            require(!impl_->services.contains(id), "SERVICE_DUPLICATE");
            const auto key = service.at("contract").get<std::string>() + "@" + std::to_string(service.at("version").get<int>());
            for (const auto& [name, method] : service.at("methods").items()) { (void)name; validate_method(method); }
            if (contracts.contains(key) && !sdk::contracts::builtins().contains(service.at("contract"))) require(contracts.at(key) == service.at("methods"), "CONTRACT_CONFLICT");
            else contracts.emplace(key, service.at("methods"));
            service["plugin_id"] = plugin->id; service["plugin_version"] = plugin->manifest.at("version");
            service["config_revision"] = selected.at("revision");
            service["config_fingerprint"] = fingerprint(selected.value("config", Json::object()).dump());
            impl_->services.emplace(id, service);
        }
        plugin->action_definitions = plugin->manifest.value("actions", Json::array());
        impl_->plugins.emplace(plugin->id, std::move(plugin));
    }
    for (const auto& [name, target] : impl_->profile.data.at("bindings").items()) {
        require(impl_->services.contains(target.get<std::string>()), "BINDING_MISSING");
        sdk::contracts::validate_role(name, impl_->services.at(target.get<std::string>()));
    }
    for (const auto& activity : impl_->profile.data.value("activities", Json::array())) {
        const auto binding_name = activity.at("binding").get<std::string>();
        require(impl_->profile.data.at("bindings").contains(binding_name), "ACTIVITY_BINDING_MISSING");
        sdk::contracts::validate_role("activity", impl_->services.at(impl_->profile.data.at("bindings").at(binding_name).get<std::string>()));
    }
    std::set<std::string> channel_instances;
    for (const auto& entry : channels()) {
        const auto identity = channel_binding(entry.at("id"));
        require(channel_instances.insert(identity.at("instance").get<std::string>()).second, "CHANNEL_INSTANCE_DUPLICATE");
    }
    std::set<std::string> remaining, ready_plugins;
    for (const auto& [id, plugin] : impl_->plugins) { (void)plugin; remaining.insert(id); }
    while (!remaining.empty()) {
        auto chosen = remaining.end();
        for (auto it = remaining.begin(); it != remaining.end(); ++it) {
            bool ready = true;
            for (const auto& dep : impl_->plugins.at(*it)->manifest.at("requires")) {
                const auto target = service(binding(dep.at("binding")));
                require(target.at("contract") == dep.at("contract") && target.at("version") == dep.at("version"), "DEPENDENCY_CONTRACT");
                if (!ready_plugins.contains(target.at("plugin_id").get<std::string>())) ready = false;
            }
            if (ready) { chosen = it; break; }
        }
        require(chosen != remaining.end(), "DEPENDENCY_CYCLE");
        impl_->order.push_back(*chosen); ready_plugins.insert(*chosen); remaining.erase(chosen);
    }
    for (const auto& id : impl_->order) {
        auto& plugin = *impl_->plugins.at(id);
        try {
            for (const auto& dep : plugin.manifest.at("requires")) require(ready(binding(dep.at("binding"))), "DEPENDENCY_NOT_READY");
            plugin.library = std::make_unique<Library>(child(impl_->profile.plugins, plugin.manifest.at("library")));
            const auto query = plugin.library->query(); plugin.api.struct_size = sizeof(dfy_plugin_api);
            require(query && query(DFY_ABI_VERSION, &plugin.api) == DFY_OK && plugin.api.struct_size == sizeof(dfy_plugin_api) &&
                plugin.api.abi_version == DFY_ABI_VERSION && plugin.api.plugin_id && plugin.api.plugin_version &&
                id == plugin.api.plugin_id && plugin.manifest.at("version") == plugin.api.plugin_version &&
                plugin.api.create && plugin.api.start && plugin.api.on_event && plugin.api.quiesce && plugin.api.stop && plugin.api.destroy, "PLUGIN_ABI_INVALID");
            plugin.host = {sizeof(dfy_host_api), DFY_ABI_VERSION, &plugin, sdk::bytes(plugin.data), sdk::bytes(plugin.instance),
                Loaded::registration, Loaded::post, Loaded::cancelled};
            plugin.status = "starting";
            plugin.worker.sync([&] {
                Errors errors; const auto config = plugin.selected.value("config", Json::object()).dump();
                require(plugin.api.create(&plugin.host, sdk::bytes(config), &plugin.object, errors.sink()) == DFY_OK && plugin.object, "PLUGIN_CREATE:" + errors.value.dump());
                plugin.started = true;
                require(plugin.api.start(plugin.object, errors.sink()) == DFY_OK, "PLUGIN_START:" + errors.value.dump());
                require(plugin.registrations.size() == plugin.manifest.at("services").size(), "REGISTRATION_INCOMPLETE");
                plugin.registering = false;
            });
            std::map<std::string, Json> staged_actions;
            for (auto action : plugin.action_definitions) {
                const auto action_id = action.at("id").get<std::string>();
                require(valid_name(action_id), "ACTION_ID_INVALID");
                require(!impl_->action_map.contains(action_id) && !staged_actions.contains(action_id), "ACTION_NAME_CONFLICT");
                require(owner(action.at("service")) == id, "ACTION_OWNER");
                require(service(action.at("service")).at("contract") == "dafeiyu.action-executor" && action.value("method", std::string("execute")) == "execute", "ACTION_EXECUTOR_REQUIRED");
                validate_method(action);
                require(action.at("revision").is_number_integer() && action.at("revision").get<int>() > 0, "ACTION_REVISION_INVALID");
                action["plugin_id"] = id; staged_actions.emplace(action_id, action);
            }
            impl_->action_map.insert(staged_actions.begin(), staged_actions.end());
            plugin.status = "ready";
        } catch (const std::exception& error) {
            plugin.diagnostic = error.what(); plugin.close(); plugin.status = "failed";
            if (plugin.selected.value("required", true)) { stop(); throw; }
        }
    }
}
void PluginRuntime::quiesce() {
    if (impl_->draining) return;
    impl_->draining = true;
    for (const auto& id : impl_->order) {
        auto& p = *impl_->plugins.at(id);
        if (p.object) p.worker.post([&p] { p.api.quiesce(p.object); });
        std::lock_guard lock(p.mutex);
        for (auto& [ticket, control] : p.calls) { (void)ticket; control.cancelled = true; }
    }
}
void PluginRuntime::stop() {
    if (!impl_) return;
    quiesce();
    for (auto it = impl_->order.rbegin(); it != impl_->order.rend(); ++it) impl_->plugins.at(*it)->close();
}
Json PluginRuntime::catalog() const {
    Json result = Json::array();
    for (const auto& [id, plugin] : impl_->plugins) result.push_back({{"id", id}, {"status", plugin->status}, {"error", plugin->diagnostic}, {"services", plugin->manifest.at("services")}});
    return result;
}
Json PluginRuntime::actions() const { Json values = Json::array(); for (const auto& [id, action] : impl_->action_map) { (void)id; values.push_back(action); } return values; }
Json PluginRuntime::activities() const {
    Json values = Json::array(); std::set<std::string> names;
    for (const auto& [id, plugin] : impl_->plugins) {
        if (plugin->status != "ready") continue;
        for (auto definition : plugin->manifest.value("activities", Json::array())) {
            require(valid_name(definition.at("id")) && names.insert(definition.at("id").get<std::string>()).second, "ACTIVITY_REGISTRATION_CONFLICT");
            require(owner(definition.at("service")) == id && service(definition.at("service")).at("contract") == "dafeiyu.activity", "ACTIVITY_OWNER_INVALID");
            definition["plugin_id"] = id; values.push_back(definition);
        }
    }
    return values;
}
Json PluginRuntime::service(const std::string& id) const { require(impl_->services.contains(id), "SERVICE_MISSING:" + id); return impl_->services.at(id); }
Json PluginRuntime::action(const std::string& id) const { require(impl_->action_map.contains(id), "ACTION_MISSING:" + id); return impl_->action_map.at(id); }
Json PluginRuntime::method(const std::string& id, const std::string& name) const { const auto descriptor = service(id); require(descriptor.at("methods").contains(name), "METHOD_MISSING"); return descriptor.at("methods").at(name); }
std::string PluginRuntime::binding(const std::string& name) const { require(impl_->profile.data.at("bindings").contains(name), "BINDING_MISSING"); return impl_->profile.data.at("bindings").at(name); }
std::string PluginRuntime::owner(const std::string& id) const { return service(id).at("plugin_id"); }
bool PluginRuntime::ready(const std::string& id) const { return impl_->plugins.at(owner(id))->status == "ready"; }
bool PluginRuntime::dependency(const std::string& id, const std::string& name) const {
    for (const auto& dep : impl_->plugins.at(id)->manifest.at("requires")) if (dep.at("binding") == name) return true;
    return false;
}
Json PluginRuntime::selection(const std::string& id) const { return impl_->plugins.at(id)->selected; }
bool PluginRuntime::plugin_ready(const std::string& id) const { const auto found = impl_->plugins.find(id); return found != impl_->plugins.end() && found->second->status == "ready"; }
Json PluginRuntime::plugin_snapshot(const std::string& plugin) const {
    Json result{{"owner", impl_->profile.data.at("owner")}, {"bindings", Json::object()}, {"services", Json::object()}, {"actions", Json::object()}};
    std::set<std::string> needed;
    for (const auto& [id, info] : impl_->services) if (info.at("plugin_id") == plugin) needed.insert(id);
    for (const auto& dep : impl_->plugins.at(plugin)->manifest.at("requires")) { const auto name = dep.at("binding").get<std::string>(); result["bindings"][name] = binding(name); needed.insert(binding(name)); }
    while (result["services"].size() < needed.size()) {
        const auto batch = needed;
        for (const auto& id : batch) if (!result["services"].contains(id)) {
            result["services"][id] = service(id);
            for (const auto& dep : impl_->plugins.at(owner(id))->manifest.at("requires")) { const auto name = dep.at("binding").get<std::string>(); result["bindings"][name] = binding(name); needed.insert(binding(name)); }
        }
    }
    return result;
}
Json PluginRuntime::channels() const {
    Json result = Json::array();
    for (const auto& [id, value] : impl_->services) { (void)id; if (value.at("contract") == "dafeiyu.channel") result.push_back(value); }
    return result;
}
Json PluginRuntime::channel_binding(const std::string& id) const {
    const auto info = service(id);
    require(info.at("contract") == "dafeiyu.channel", "CHANNEL_CONTRACT_INVALID");
    const auto selected = selection(owner(id));
    require(selected.contains("channels") && selected.at("channels").contains(id), "CHANNEL_IDENTITY_REQUIRED");
    const auto& identity = selected.at("channels").at(id);
    const auto instance = identity.at("instance").get<std::string>(), account = identity.at("account").get<std::string>();
    require(!instance.empty() && instance.size() <= 160 && !account.empty() && account.size() <= 160, "CHANNEL_IDENTITY_INVALID");
    return {{"service", id}, {"plugin", owner(id)}, {"instance", instance}, {"account", account},
        {"provider_version", info.at("plugin_version")}, {"config_revision", info.at("config_revision")}, {"config_fingerprint", info.at("config_fingerprint")}};
}
Json PluginRuntime::snapshot(const Json& action_ids, std::optional<std::string> channel) const {
    Json result{{"owner", impl_->profile.data.at("owner")}, {"bindings", Json::object()}, {"services", Json::object()}, {"actions", Json::object()}};
    std::set<std::string> needed;
    for (const auto& [name, target] : impl_->profile.data.at("bindings").items()) {
        if (name == "model" || name == "memory" || name == "channel" || name == "strategy" || name.starts_with("context.") || name.starts_with("model.")) {
            if (name == "channel" && channel.has_value()) {
                if (!channel->empty()) { result["bindings"][name] = *channel; needed.insert(*channel); }
                continue;
            }
            result["bindings"][name] = target; needed.insert(target.get<std::string>());
        }
    }
    for (const auto& id : action_ids) {
        Json info;
        if (impl_->action_map.contains(id.get<std::string>())) info = action(id);
        else for (const auto& [owner_id, plugin] : impl_->plugins) {
            for (auto declared : plugin->manifest.value("actions", Json::array())) if (declared.at("id") == id) {
                declared["plugin_id"] = owner_id; info = declared;
            }
        }
        require(!info.is_null(), "PROFILE_ACTION_UNDECLARED");
        result["actions"][id.get<std::string>()] = info; needed.insert(info.at("service").get<std::string>());
    }
    while (result.at("services").size() < needed.size()) {
        const auto batch = needed;
        for (const auto& id : batch) {
            if (result.at("services").contains(id)) continue;
            result["services"][id] = service(id);
            for (const auto& dep : impl_->plugins.at(owner(id))->manifest.at("requires")) {
                const auto name = dep.at("binding").get<std::string>(); const auto target = binding(name);
                result["bindings"][name] = target; needed.insert(target);
            }
        }
    }
    return result;
}
bool PluginRuntime::matches(const Json& snapshot_value) const {
    if (snapshot_value.at("owner") != impl_->profile.data.at("owner")) return false;
    for (const auto& [id, info] : snapshot_value.at("services").items()) {
        if (!impl_->services.contains(id)) return false;
        auto current = service(id), saved = info;
        current.erase("description"); saved.erase("description");
        if (current != saved) return false;
    }
    return true;
}
void PluginRuntime::invoke(uint64_t call, const std::string& id, const std::string& name, const Json& input, const Json& context, SteadyClock::time_point deadline) {
    require(!impl_->draining && ready(id), "SERVICE_NOT_READY");
    auto& p = *impl_->plugins.at(owner(id));
    { std::lock_guard lock(p.mutex); require(p.calls.size() < 256, "PLUGIN_CALL_LIMIT"); p.calls.emplace(call, Control{false, deadline}); }
    send(p.id, {{"type", "call"}, {"call_id", call}, {"service", id}, {"method", name}, {"input", input}, {"context", context}});
}
void PluginRuntime::send(const std::string& id, const Json& event) {
    auto& p = *impl_->plugins.at(id); require(p.object != nullptr, "PLUGIN_NOT_RUNNING");
    p.worker.post([&p, event] {
        Errors errors; const auto body = event.dump();
        if (p.api.on_event(p.object, sdk::bytes(body), errors.sink()) != DFY_OK) {
            const auto error = errors.value.is_null() ? sdk::error("PLUGIN_EVENT_FAILURE") : errors.value;
            if (event.value("type", std::string{}) == "call") (void)p.receive({p.id, {{"type", "complete"}, {"call_id", event.at("call_id")}, {"error", error}}});
            else (void)p.receive({p.id, {{"type", "plugin_error"}, {"error", error}}});
        }
    });
}
void PluginRuntime::cancel(uint64_t call) { for (auto& [id, p] : impl_->plugins) { (void)id; std::lock_guard lock(p->mutex); if (p->calls.contains(call)) p->calls.at(call).cancelled = true; } }
void PluginRuntime::release(uint64_t call) { for (auto& [id, p] : impl_->plugins) { (void)id; std::lock_guard lock(p->mutex); p->calls.erase(call); } }
void PluginRuntime::allow_job(const std::string& plugin, const std::string& job) { auto& p = *impl_->plugins.at(plugin); std::lock_guard lock(p.mutex); p.jobs.insert(job); }
}
