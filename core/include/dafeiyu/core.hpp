#pragma once
#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace dfy {
using Json = sdk::Json;
using Path = std::filesystem::path;
Json read_json(const Path& path);
std::string read_text(const Path& path);

// Single owner thread; calls finish before shutdown. Cancellation can come from another thread.
class PluginManager {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    PluginManager();
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    void load(const Path& directory, const Path& data, const std::string& instance, const Json& selection);
    void shutdown() noexcept;
    size_t service_count() const;
    Json catalog() const;
    Json describe(const std::string& service) const;
    std::vector<Json> call(const std::string& service, const std::string& method, const Json& input,
        const Json& context, std::stop_token stop = {}, std::chrono::milliseconds timeout = std::chrono::seconds(30));
    Json one(const std::string& service, const std::string& method, const Json& input, const Json& context,
        std::stop_token stop = {});
};

class InstanceLock {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit InstanceLock(const Path& directory);
    ~InstanceLock();
};
class Store {
    InstanceLock lock_;
    Database db_;
    std::string instance_;
public:
    explicit Store(const Path& directory);
    const std::string& instance_id() const { return instance_; }
    Json find(const std::string& request_id);
    Json create(const std::string& request_id, const Json& inbound, const Json& bindings, const std::string& foundation);
    void commit(const Json& run, const std::string& type, const Json& payload = Json::object());
    Json events(const std::string& request_id);
};
struct Profile {
    Json data;
    Path plugins;
    std::string foundation;
    static Profile open(const Path& file);
};
struct RunOptions {
    bool pause_after_action = false;
    std::stop_token stop;
    // Observes committed milestones; useful for CLI progress and process-crash tests.
    std::function<void(std::string_view)> observer;
};
class Runtime {
    Profile profile_;
    Path directory_;
    Store store_;
    PluginManager plugins_;
    Json bindings() const;
    Json context(const Json& run, const std::string& operation) const;
    void commit(Json& run, const std::string& event, const Json& payload, const RunOptions& options);
    Json drive(Json run, const RunOptions& options);
public:
    Runtime(Profile profile, Path directory);
    Json submit(const std::string& request_id, const std::string& message, const RunOptions& options = {});
    Json resume(const std::string& request_id, const RunOptions& options = {});
    Json status(const std::string& request_id) { return store_.find(request_id); }
    Json events(const std::string& request_id) { return store_.events(request_id); }
    Json plugins() const { return plugins_.catalog(); }
};
}
