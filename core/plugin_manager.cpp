#include <dafeiyu/core.hpp>
#include <dafeiyu/action_schema.hpp>
#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dfy {
std::string read_text(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("FILE_OPEN: " + sdk::path_text(path));
    const auto size = std::filesystem::file_size(path);
    if (size > 4 * 1024 * 1024) throw std::runtime_error("FILE_TOO_LARGE");
    std::string text(static_cast<size_t>(size), '\0');
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("FILE_READ");
    return text;
}
Json read_json(const Path& path) { return Json::parse(read_text(path)); }
namespace {
bool valid_id(const std::string& id) {
    static const std::regex pattern("[a-z][a-z0-9._-]{0,79}");
    return std::regex_match(id, pattern);
}
Path child_path(const Path& parent, const std::string& name) {
    const auto root = std::filesystem::canonical(parent);
    const auto path = std::filesystem::canonical(root / sdk::path_from_utf8(name));
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") throw std::runtime_error("PLUGIN_PATH_ESCAPE");
    return path;
}
struct Library {
#ifdef _WIN32
    HMODULE handle{};
    explicit Library(const Path& file) : handle(LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
        if (!handle) throw std::runtime_error("PLUGIN_LOAD: " + std::to_string(GetLastError()));
    }
    ~Library() { if (handle) FreeLibrary(handle); }
    dfy_query_fn query() { return reinterpret_cast<dfy_query_fn>(GetProcAddress(handle, "dfy_plugin_query")); }
#else
    void* handle{};
    explicit Library(const Path& file) : handle(dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL)) {
        if (!handle) throw std::runtime_error(std::string("PLUGIN_LOAD: ") + dlerror());
    }
    ~Library() { if (handle) dlclose(handle); }
    dfy_query_fn query() { return reinterpret_cast<dfy_query_fn>(dlsym(handle, "dfy_plugin_query")); }
#endif
};
struct Collector {
    std::vector<Json> values;
    size_t bytes{};
    std::string error;
    static int32_t DFY_CALL emit(void* user, dfy_bytes value) noexcept {
        auto& self = *static_cast<Collector*>(user);
        try {
            if (self.values.size() >= 256 || value.size > 1024 * 1024 - self.bytes) throw std::runtime_error("OUTPUT_LIMIT");
            self.bytes += static_cast<size_t>(value.size);
            self.values.push_back(Json::parse(sdk::text(value)));
            return DFY_OK;
        } catch (const std::exception& e) { self.error = e.what(); return DFY_ERROR; }
        catch (...) { self.error = "OUTPUT_INVALID"; return DFY_ERROR; }
    }
    dfy_sink sink() { return {this, emit}; }
    std::string message() const { return error.empty() ? (values.empty() ? "PLUGIN_FAILURE" : values.back().dump()) : error; }
};
struct Cancellation {
    std::stop_token stop;
    std::chrono::steady_clock::time_point deadline;
    static int32_t DFY_CALL check(void* raw) noexcept {
        const auto& self = *static_cast<Cancellation*>(raw);
        return self.stop.stop_requested() || std::chrono::steady_clock::now() >= self.deadline;
    }
};
struct Loaded {
    Json manifest;
    std::unique_ptr<Library> library;
    dfy_plugin_api api{};
    dfy_host_api host{};
    std::string data_path, instance_id;
    std::set<std::string> registered;
    bool registering = true;
    bool started = false;
    void* object{};
    ~Loaded() {
        if (object) { if (started) api.stop(object); api.destroy(object); }
    }
    static int32_t DFY_CALL provide(void* user, dfy_bytes id) noexcept {
        auto& self = *static_cast<Loaded*>(user);
        try {
            const auto name = sdk::text(id);
            if (!self.registering || self.registered.contains(name)) return DFY_ERROR;
            for (const auto& service : self.manifest.at("services")) {
                if (service.at("id") == name) { self.registered.insert(name); return DFY_OK; }
            }
        } catch (...) {}
        return DFY_ERROR;
    }
};
}
struct PluginManager::Impl {
    std::vector<std::unique_ptr<Loaded>> loaded;
    std::map<std::string, std::pair<Loaded*, Json>> services;
};
PluginManager::PluginManager() : impl_(std::make_unique<Impl>()) {}
PluginManager::~PluginManager() { shutdown(); }
void PluginManager::shutdown() noexcept {
    impl_->services.clear();
    while (!impl_->loaded.empty()) impl_->loaded.pop_back();
}
size_t PluginManager::service_count() const { return impl_->services.size(); }
void PluginManager::load(const Path& directory, const Path& data, const std::string& instance, const Json& selection) {
    if (!impl_->loaded.empty()) throw std::runtime_error("PLUGINS_ALREADY_LOADED");
    struct Pending { Json manifest, config; Path path; };
    std::map<std::string, Pending> pending;
    std::map<std::string, std::string> owners;
    for (const auto& selected : selection) {
        const auto manifest = read_json(child_path(directory, selected.at("manifest").get<std::string>()));
        const auto id = manifest.at("id").get<std::string>();
        if (!valid_id(id) || manifest.at("abi_version") != DFY_ABI_VERSION || pending.contains(id)) throw std::runtime_error("PLUGIN_MANIFEST_INVALID");
        if (!manifest.at("version").is_string() || !manifest.at("requires").is_array() || !manifest.at("services").is_array()) throw std::runtime_error("PLUGIN_MANIFEST_INVALID");
        for (const auto& service : manifest.at("services")) {
            const auto name = service.at("id").get<std::string>();
            const auto kind = service.at("kind").get<std::string>();
            if (!valid_id(name) || owners.contains(name) || service.at("version") != 1 ||
                (kind != "model" && kind != "channel" && kind != "memory" && kind != "action")) throw std::runtime_error("SERVICE_MANIFEST_INVALID");
            if (kind == "action") {
                sdk::validate_action_schema(service.at("input_schema"));
                const auto effect = service.at("effects").get<std::string>();
                if (effect != "read" && effect != "local-write" && effect != "external-write") throw std::runtime_error("ACTION_EFFECT_INVALID");
            }
            owners.emplace(name, id);
        }
        pending.emplace(id, Pending{manifest, selected.value("config", Json::object()), child_path(directory, manifest.at("library").get<std::string>())});
    }
    for (const auto& [id, plugin] : pending) for (const auto& dependency : plugin.manifest.at("requires")) {
        if (!owners.contains(dependency.get<std::string>())) throw std::runtime_error("PLUGIN_DEPENDENCY_MISSING: " + id);
    }
    std::vector<std::string> order;
    std::set<std::string> ready;
    auto remaining = pending;
    while (!remaining.empty()) {
        auto next = std::find_if(remaining.begin(), remaining.end(), [&](const auto& item) {
            for (const auto& requirement : item.second.manifest.at("requires")) if (!ready.contains(requirement.get<std::string>())) return false;
            return true;
        });
        if (next == remaining.end()) throw std::runtime_error("PLUGIN_DEPENDENCY_CYCLE");
        order.push_back(next->first);
        for (const auto& service : next->second.manifest.at("services")) ready.insert(service.at("id").get<std::string>());
        remaining.erase(next);
    }
    try {
        for (const auto& id : order) {
            const auto& source = pending.at(id);
            auto plugin = std::make_unique<Loaded>();
            plugin->manifest = source.manifest;
            plugin->library = std::make_unique<Library>(source.path);
            auto query = plugin->library->query();
            plugin->api.struct_size = sizeof(dfy_plugin_api);
            if (!query || query(DFY_ABI_VERSION, &plugin->api) != DFY_OK || plugin->api.struct_size < sizeof(dfy_plugin_api) ||
                plugin->api.abi_version != DFY_ABI_VERSION || !plugin->api.plugin_id || id != plugin->api.plugin_id ||
                !plugin->api.plugin_version || source.manifest.at("version") != plugin->api.plugin_version ||
                !plugin->api.create || !plugin->api.start || !plugin->api.invoke || !plugin->api.stop || !plugin->api.destroy)
                throw std::runtime_error("PLUGIN_ABI_OR_IDENTITY_MISMATCH");
            const auto path = std::filesystem::absolute(data / id);
            std::filesystem::create_directories(path);
            plugin->data_path = sdk::path_text(path);
            plugin->instance_id = instance;
            plugin->host = {sizeof(dfy_host_api), DFY_ABI_VERSION, plugin.get(), sdk::bytes(plugin->data_path), sdk::bytes(plugin->instance_id), Loaded::provide};
            Collector errors;
            const auto config = source.config.dump();
            if (plugin->api.create(&plugin->host, sdk::bytes(config), &plugin->object, errors.sink()) != DFY_OK || !plugin->object)
                throw std::runtime_error("PLUGIN_CREATE: " + errors.message());
            plugin->registering = false;
            if (plugin->registered.size() != source.manifest.at("services").size()) throw std::runtime_error("PLUGIN_REGISTRATION_INCOMPLETE");
            // Even a partially started plugin receives stop before destruction.
            plugin->started = true;
            if (plugin->api.start(plugin->object, errors.sink()) != DFY_OK) throw std::runtime_error("PLUGIN_START: " + errors.message());
            impl_->loaded.push_back(std::move(plugin));
        }
        for (const auto& plugin : impl_->loaded) for (auto service : plugin->manifest.at("services")) {
            service["plugin_id"] = plugin->manifest.at("id");
            service["plugin_version"] = plugin->manifest.at("version");
            impl_->services.emplace(service.at("id").get<std::string>(), std::make_pair(plugin.get(), service));
        }
    } catch (...) { shutdown(); throw; }
}
Json PluginManager::catalog() const {
    Json result = Json::array();
    for (const auto& [id, entry] : impl_->services) { (void)id; result.push_back(entry.second); }
    return result;
}
Json PluginManager::describe(const std::string& service) const {
    const auto found = impl_->services.find(service);
    if (found == impl_->services.end()) throw std::runtime_error("SERVICE_UNAVAILABLE: " + service);
    return found->second.second;
}
std::vector<Json> PluginManager::call(const std::string& service, const std::string& method, const Json& input,
    const Json& context, std::stop_token stop, std::chrono::milliseconds timeout) {
    describe(service);
    auto& plugin = *impl_->services.at(service).first;
    Cancellation cancellation{stop, std::chrono::steady_clock::now() + timeout};
    if (Cancellation::check(&cancellation)) throw sdk::Cancelled{};
    const auto encoded = input.dump(), scope = context.dump();
    dfy_call call{sizeof(dfy_call), sdk::bytes(service), sdk::bytes(method), sdk::bytes(encoded), sdk::bytes(scope), &cancellation, Cancellation::check};
    Collector output;
    const auto status = plugin.api.invoke(plugin.object, &call, output.sink());
    if (status == DFY_CANCELLED) throw sdk::Cancelled{};
    if (status != DFY_OK || !output.error.empty()) throw std::runtime_error("PLUGIN_CALL: " + output.message());
    // A late result can contain a confirmed side effect; return it before cancellation admission.
    return output.values;
}
Json PluginManager::one(const std::string& service, const std::string& method, const Json& input, const Json& context, std::stop_token stop) {
    const auto values = call(service, method, input, context, stop);
    if (values.size() != 1) throw std::runtime_error("EXPECTED_ONE_PLUGIN_RESULT");
    return values.front();
}
}
