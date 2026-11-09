#pragma once
#include <dafeiyu/value.hpp>
#include <memory>

namespace dfy {
using Json = sdk::Json;
using Path = std::filesystem::path;
struct Profile {
    Json data;
    Path plugins;
    std::string foundation;
    static Profile open(const Path& file);
};
// Commands may be submitted concurrently. The owner serializes shutdown/destruction.
// The instance owns the only state-writing thread.
class InstanceRuntime {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    InstanceRuntime(Profile profile, Path state);
    ~InstanceRuntime();
    InstanceRuntime(const InstanceRuntime&) = delete;
    InstanceRuntime& operator=(const InstanceRuntime&) = delete;
    Json request(const std::string& command, const Json& arguments = Json::object());
    void shutdown();
};
}
