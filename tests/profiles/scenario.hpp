#pragma once
#include <dafeiyu/core.hpp>
#include <fstream>
#include <set>

namespace dfy::test {
inline bool selected(const Profile& profile, const std::string& role) {
    for (const auto& plugin : profile.data.at("plugins"))
        if (plugin.at("manifest") == "dfy_fixture_" + role + ".json") return true;
    return false;
}
inline void append_unique(Json& list, const Json& value) {
    if (std::find(list.begin(), list.end(), value) == list.end()) list.push_back(value);
}
inline void use_fixture(Profile& profile, const Json& catalog, const std::string& role) {
    if (selected(profile, role)) return;
    if (role == "action_search") use_fixture(profile, catalog, "search_primary");
    if (role == "action_remember") use_fixture(profile, catalog, "memory_minimal");
    if (role == "activity_reflection") use_fixture(profile, catalog, "action_remember");
    bool replaced = false;
    for (auto& plugin : profile.data.at("plugins")) if (plugin.at("manifest") == "dfy_" + role + ".json") {
        plugin = catalog.at(role); replaced = true; break;
    }
    if (!replaced) profile.data["plugins"].push_back(catalog.at(role));
    if (role == "search_primary") profile.data["bindings"]["search"] = "search.primary";
    if (role == "memory_minimal") profile.data["bindings"]["memory.changes"] = "memory.changes";
    if (role == "activity_reflection") profile.data["bindings"]["reflection"] = "activity.reflection";
    if (role == "context_clock") profile.data["bindings"]["context.clock"] = "context.clock";
    std::ifstream manifest(profile.plugins / sdk::path_from_utf8(catalog.at(role).at("manifest").get<std::string>()));
    const auto descriptor = Json::parse(manifest);
    for (const auto& action : descriptor.value("actions", Json::array())) {
        append_unique(profile.data["allowed_actions"], action.at("id"));
        append_unique(profile.data["allowed_effects"], action.at("effects"));
        append_unique(profile.data["resources"], action.at("resource"));
    }
    if (role == "memory_minimal") append_unique(profile.data["resources"], "memory");
    if (role == "search_primary" || role == "search_secondary") append_unique(profile.data["resources"], "web");
}
inline Profile scenario(Profile profile, const Path& catalog_path, const std::string& name) {
    std::ifstream file(catalog_path); const auto catalog = Json::parse(file);
    profile.data["activities"] = Json::array();
    const std::set<std::string> scripted{"provider_swap", "nested_memory", "unknown_effect", "revocation", "cancel", "steer", "responsive", "deadline", "job", "job_early", "activity", "steer_delivery", "binding_change", "config_change", "unrelated_config", "host.resident", "host.action", "host.job"};
    if (scripted.contains(name)) use_fixture(profile, catalog, "model_scripted");
    if (name == "provider_swap" || name == "steer" || name == "steer_delivery" || name == "schema" || name == "contract_conflict" || name == "host.resident") use_fixture(profile, catalog, "action_search");
    if (name == "provider_swap" || name == "contract_conflict" || name == "unrelated_config") use_fixture(profile, catalog, "search_secondary");
    if (name == "nested_memory") use_fixture(profile, catalog, "action_remember");
    if (name == "job" || name == "job_early" || name == "host.job") use_fixture(profile, catalog, "action_background");
    if (name == "unknown_effect" || name == "binding_change" || name == "config_change" || name == "unrelated_config" || name == "host.action") use_fixture(profile, catalog, "action_write_demo");
    if (name == "steer_delivery" || name == "channel_input" || name == "host.delivery") use_fixture(profile, catalog, "channel_simulator");
    if (name == "activity") use_fixture(profile, catalog, "activity_reflection");
    if (name == "behavior_plugin") use_fixture(profile, catalog, "action_behavior");
    if (name == "context_subscription") use_fixture(profile, catalog, "context_clock");
    if (name == "duplicate_terminal" || name == "nested_denied" || name == "forged_scope" || name == "startup_rollback" || name == "degraded") use_fixture(profile, catalog, "memory_minimal");
    return profile;
}
inline std::string manifest_name(const Profile& profile, const std::string& role) {
    for (const auto& item : profile.data.at("plugins")) {
        const auto name = item.at("manifest").get<std::string>();
        if (name == "dfy_" + role + ".json" || name == "dfy_fixture_" + role + ".json") return name;
    }
    throw std::runtime_error("TEST_PLUGIN_MISSING: " + role);
}
}
