#pragma once
#include "common.hpp"
#include <memory>

namespace dfy {
struct PluginMessage { std::string plugin; Json body; };
class PluginRuntime {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    using Receiver = std::function<int(PluginMessage)>;
    PluginRuntime(const Profile& profile, const Path& state, const std::string& instance, Receiver receive);
    ~PluginRuntime();
    void start();
    void quiesce();
    void stop();
    Json catalog() const;
    Json actions() const;
    Json activities() const;
    Json service(const std::string& id) const;
    Json action(const std::string& id) const;
    Json method(const std::string& service, const std::string& method) const;
    std::string binding(const std::string& name) const;
    std::string owner(const std::string& service) const;
    bool ready(const std::string& service) const;
    bool dependency(const std::string& plugin, const std::string& binding) const;
    Json selection(const std::string& plugin) const;
    Json snapshot(const Json& action_ids) const;
    bool matches(const Json& snapshot) const;
    void invoke(uint64_t call, const std::string& service, const std::string& method, const Json& input, const Json& context, SteadyClock::time_point deadline);
    void send(const std::string& plugin, const Json& event);
    void cancel(uint64_t call);
    void release(uint64_t call);
    void allow_job(const std::string& plugin, const std::string& job);
};
}
