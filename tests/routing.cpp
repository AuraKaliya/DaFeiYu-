#include <dafeiyu/core.hpp>
#include <chrono>
#include <iostream>
#include <thread>
using namespace dfy;
namespace {
void check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
template<class Fn> Json wait(Fn fn) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < end) { auto result = fn(); if (!result.is_null()) return result; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    throw std::runtime_error("ROUTING_TIMEOUT");
}
Json events(InstanceRuntime& host) {
    Json all = Json::array(); uint64_t after = 0;
    for (;;) { const auto page = host.request("events", {{"after", after}, {"limit", 200}});
        for (const auto& event : page.at("events")) all.push_back(event);
        if (page.at("cursor") == after) return all; after = page.at("cursor"); }
}
Json& channel(Profile& profile) {
    for (auto& item : profile.data["plugins"]) if (item.at("manifest") == "dfy_fixture_channel_dual.json") return item;
    throw std::runtime_error("CHANNEL_CONFIG_MISSING");
}
Profile dual(Profile profile) {
    for (auto& item : profile.data["plugins"]) if (item.at("manifest") == "dfy_channel_simulator.json") {
        item["manifest"] = "dfy_fixture_channel_dual.json";
        item["config"] = {{"services", Json::array({"channel.simulator", "channel.second"})}};
        item["channels"] = {{"channel.simulator", {{"instance", "first"}, {"account", "account-a"}, {"allow_reply", true}}},
            {"channel.second", {{"instance", "second"}, {"account", "account-b"}, {"allow_reply", true}}}};
    }
    return profile;
}
std::string run_for(InstanceRuntime& host, const std::string& key) {
    const auto input = wait([&] { for (const auto& event : events(host))
        if (event.at("type") == "input.accepted" && event.at("payload").at("key") == key) return event.at("payload"); return Json{}; });
    return wait([&] { const auto current = host.request("input.get", {{"id", input.at("id")}}); return current.at("run_id"); }).get<std::string>();
}
Json finished(InstanceRuntime& host, const std::string& run) {
    return wait([&] { const auto state = host.request("run.get", {{"id", run}});
        if (state.at("execution") == "failed") throw std::runtime_error(state.at("error").dump());
        return state.at("execution") == "completed" ? state : Json{}; });
}
void check_delivery(InstanceRuntime& host, const Json& run, const std::string& service) {
    bool found = false;
    for (const auto& operation : host.request("operation.list")) if (operation.at("run_id") == run.at("id") && operation.at("kind") == "delivery") {
        found = true; check(operation.at("descriptor").at("service") == service, "wrong delivery provider");
        check(operation.at("input").at("target") == "room-7", "lost external conversation id");
        check(operation.at("input").at("binding") == run.at("delivery_binding"), "delivery binding changed");
    }
    check(found, "no delivery operation");
}
void execute(const std::string& name, Profile profile, const Path& state) {
    profile = dual(std::move(profile));
    if (name == "two_channels") {
        channel(profile)["config"]["inputs"] = Json::array({
            {{"key", "one"}, {"channel", "channel.simulator"}, {"conversation", "room-7"}, {"text", "first"}},
            {{"key", "two"}, {"channel", "channel.second"}, {"conversation", "room-7"}, {"text", "second"}}});
        InstanceRuntime host(profile, state);
        const auto first = finished(host, run_for(host, "one")), second = finished(host, run_for(host, "two"));
        check(first.at("conversation") != second.at("conversation"), "channel namespaces collided");
        check(first.at("conversation_key").at("account") == "account-a" && second.at("conversation_key").at("account") == "account-b", "account identity lost");
        check_delivery(host, first, "channel.simulator"); check_delivery(host, second, "channel.second");
    } else if (name == "queued_route" || name == "account_change" || name == "authorization_generation" || name == "rename_generation") {
        profile.data["bindings"]["channel"] = "channel.second";
        { InstanceRuntime host(profile, state); host.request("behavior.set", {{"expected_revision", 1}, {"mode", "resting"}});
          host.request("input.accept", {{"key", "queued"}, {"text", "queued input"}, {"conversation", "room-7"}}); }
        profile.data["bindings"]["channel"] = "channel.simulator";
        if (name == "account_change") channel(profile)["channels"]["channel.second"]["account"] = "another-account";
        if (name == "authorization_generation") channel(profile)["channels"]["channel.second"]["allow_reply"] = false;
        if (name == "rename_generation") channel(profile)["channels"]["channel.second"]["instance"] = "renamed";
        std::string id;
        { InstanceRuntime host(profile, state); host.request("behavior.set", {{"expected_revision", 2}, {"mode", "available"}});
          id = run_for(host, "queued");
          if (name == "queued_route") check_delivery(host, finished(host, id), "channel.second");
          else {
              const auto run = wait([&] { auto value = host.request("run.get", {{"id", id}}); return value.at("error").is_null() ? Json{} : value; });
              check(run.at("error").at("code") == (name == "account_change" || name == "rename_generation" ? "DELIVERY_BINDING_CHANGED" : "DELIVERY_AUTHORITY_CHANGED"), run.at("error").dump());
              for (const auto& op : host.request("operation.list")) check(op.at("kind") != "delivery", "revoked route dispatched");
          }
        }
        if (name == "authorization_generation" || name == "rename_generation") {
            channel(profile)["channels"]["channel.second"]["allow_reply"] = true;
            channel(profile)["channels"]["channel.second"]["instance"] = "second";
            InstanceRuntime host(profile, state); host.request("run.resume", {{"id", id}});
            wait([&] { auto value = host.request("run.get", {{"id", id}}); return value.at("execution") == "waiting" ? value : Json{}; });
            for (const auto& op : host.request("operation.list")) check(op.at("kind") != "delivery", "old permission revived after regrant");
        }
    } else if (name == "foreign_channel") {
        channel(profile)["config"]["inputs"] = Json::array({{{"key", "forged"}, {"channel", "model.scripted"}, {"conversation", "room-7"}, {"text", "bad"}}});
        InstanceRuntime host(profile, state);
        const auto rejected = wait([&] { for (const auto& e : events(host)) if (e.at("type") == "plugin.request_rejected") return e.at("payload"); return Json{}; });
        check(rejected.at("error").at("code") == "INPUT_CHANNEL_OWNER_DENIED", rejected.dump());
        check(host.request("run.list").empty(), "foreign service created a run");
    } else if (name == "cross_channel_steer") {
        std::string id;
        { InstanceRuntime host(profile, state); host.request("input.accept", {{"key", "original"}, {"text", "first"}, {"conversation", "room-7"}});
          id = run_for(host, "original"); (void)finished(host, id); }
        channel(profile)["config"]["inputs"] = Json::array({{{"key", "cross"}, {"channel", "channel.second"}, {"conversation", "room-7"}, {"text", "steer"}, {"run_id", id}}});
        InstanceRuntime host(profile, state);
        const auto rejected = wait([&] { for (const auto& e : events(host)) if (e.at("type") == "input.rejected" && e.at("payload").at("key") == "cross") return e.at("payload"); return Json{}; });
        check(rejected.at("error").at("code") == "STEER_SCOPE_DENIED", rejected.dump());
        check(host.request("run.get", {{"id", id}}).at("intent_revision") == 1, "foreign channel changed intent");
    } else throw std::runtime_error("UNKNOWN_ROUTING_CASE");
}
}
int main(int argc, char** argv) {
    try { if (argc != 4) return 2;
        const auto state = sdk::path_from_utf8(argv[3]) / (std::string(argv[1]) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        execute(argv[1], Profile::open(sdk::path_from_utf8(argv[2])), state);
        std::cout << "PASS " << argv[1] << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
