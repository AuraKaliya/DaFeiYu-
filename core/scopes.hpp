#pragma once
#include "policy.hpp"
#include "conversations.hpp"

namespace dfy {
class ScopeManager {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    PolicyGate& policy_;
    Conversations& conversations_;
    const Profile& profile_;
    const std::string epoch_ = identifier();
    std::map<std::string, Json> scopes_, registrations_;
    Json permissions(const std::string& plugin, const std::string& kind) const;
    bool current(const Json& scope);
    Json descriptor(const Json& scope) const;
    std::string activate(Json scope, bool restored);
    void close_impl(const std::string& id, const std::string& reason, bool suspend);
public:
    std::function<void(const Json&)> released;
    ScopeManager(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy, Conversations& conversations, const Profile& profile)
        : store_(store), writer_(store.claim("scopes", {"scope", "registration"})), plugins_(plugins), calls_(calls), policy_(policy), conversations_(conversations), profile_(profile) {}
    void restore();
    Json open(const std::string& plugin, const Json& request);
    Json owned(const std::string& plugin, const std::string& id);
    bool valid(const std::string& id);
    Json register_resource(const std::string& plugin, const std::string& scope, const std::string& key, const std::string& kind, const Json& value);
    Json release(const std::string& plugin, const std::string& id);
    Json close(const std::string& plugin, const std::string& id);
    void refresh();
    void shutdown();
    void request(const PluginMessage& message);
    Json read_state(const std::string& plugin, const std::string& scope, const std::string& object);
    Json registration(const std::string& token) const;
    Json registrations() const { Json result = Json::array(); for (const auto& [id, item] : registrations_) { (void)id; result.push_back(item); } return result; }
    bool registration_active(const std::string& token);
    bool contains(const Json& scope, const Json& run) const;
    std::string action_registration(const Json& action, const Json& run);
    std::vector<std::pair<std::string, std::string>> contexts(const Json& run);
    Json metrics() const { return {{"scopes", scopes_.size()}, {"registrations", registrations_.size()}}; }
};
}
