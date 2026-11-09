#include <dafeiyu/core.hpp>
#include <dafeiyu/sqlite.hpp>
#include "profiles/scenario.hpp"
#include <chrono>
#include <iostream>
#include <thread>
using namespace dfy;
namespace {
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class Fn> Json wait(Fn fn) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < until) { auto value = fn(); if (!value.is_null()) return value; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    throw std::runtime_error("SCOPE_TEST_TIMEOUT");
}
Json observations(const Path& state, const std::string& type) {
    Database db(state / "plugins/test.scopes/observations.sqlite3");
    auto statement = db.prepare("SELECT body FROM observations WHERE json_extract(body,'$.type')=? ORDER BY seq"); statement.bind(1, type);
    Json result = Json::array(); while (statement.step()) result.push_back(Json::parse(statement.column(0))); return result;
}
Json run(InstanceRuntime& host, const std::string& key, const std::string& room = "a", const std::string& text = "open", const std::string& channel = "") {
    const auto receipt = host.request("input.accept", {{"key", key}, {"text", text}, {"conversation", room}, {"channel", channel}});
    const auto id = wait([&] { return host.request("input.get", {{"id", receipt.at("id")}}).at("run_id"); });
    return wait([&] { const auto value = host.request("run.get", {{"id", id}});
        return value.at("execution") == "completed" || value.at("execution") == "failed" || value.at("execution") == "waiting" && value.at("phase") == "wait" ? value : Json{}; });
}
void complete(const Json& run) { check(run.at("execution") == "completed", "run did not complete: " + run.value("error", Json{}).dump()); }
bool visible(const Json& run, const std::string& action) { for (const auto& item : run.at("request").at("actions")) if (item.at("id") == action) return true; return false; }
void scenario(const std::string& name, Profile profile, const Path& state, const Path& catalog) {
    const std::string kind = name == "instance" || name == "subscription_restart" || name == "scope_budget" ? "instance" : name == "run" || name == "run_cancel" || name == "run_restart" ? "run" : name == "call" ? "call" : "conversation";
    Json configuration{{"kind", kind}, {"probe_denied", true}, {"ack", name != "subscription_restart"}};
    if (name == "scope_budget") profile.data["limits"]["scope_calls"] = 1;
    if (name == "channels") for (auto& item : profile.data["plugins"]) if (item.at("manifest") == "dfy_channel_simulator.json") {
        item["manifest"] = "dfy_fixture_channel_dual.json"; item["config"] = {{"services", Json::array({"channel.simulator", "channel.second"})}};
        item["channels"] = {{"channel.simulator", {{"instance", "first"}, {"account", "a"}, {"allow_reply", true}}}, {"channel.second", {{"instance", "second"}, {"account", "b"}, {"allow_reply", true}}}};
    }
    if (name.starts_with("fallback")) {
        configuration["kind"] = "disabled";
        configuration["failure"] = name == "fallback_malformed" ? "malformed" : name == "fallback_deadline" ? "deadline" : "error";
        if (name == "fallback_denied") configuration["fallback"] = "model.unknown";
        if (name == "fallback_budget") { configuration["fallback"] = "model"; profile.data["limits"]["model_attempts"] = 2; }
        if (name == "fallback_deadline") profile.data["limits"]["call_timeout_ms"] = 220;
        profile.data["bindings"]["strategy"] = "fixture.strategy";
        profile.data["bindings"]["model"] = "fixture.model.primary";
        profile.data["bindings"]["model.backup"] = "fixture.model.backup";
    } else profile.data["bindings"]["context.bootstrap"] = "fixture.context.bootstrap";
    profile.data["plugins"].push_back({{"manifest", "dfy_fixture_scopes.json"}, {"revision", "1"}, {"config", configuration},
        {"observe", Json::array({"run.accepted", "run.completed", "operation.settled"})},
        {"scope_permissions", {{kind, {{"resources", Json::array({"instance"})}, {"registrations", Json::array({"action", "context", "subscription"})}}}}}});
    profile.data["allowed_actions"].push_back("fixture.number"); profile.data["allowed_actions"].push_back("fixture.text");
    profile.data["allowed_effects"].push_back("local-write");
    if (name == "run_cancel" || name == "run_restart") {
        std::ifstream file(catalog); test::use_fixture(profile, Json::parse(file), "model_scripted");
        for (auto& item : profile.data["plugins"]) if (item.at("manifest") == "dfy_fixture_model_scripted.json") item["config"]["delay_ms"] = 1000;
    }
    if (name == "action_number" || name == "action_text" || name == "action_schema") {
        std::ifstream file(catalog); const auto fixtures = Json::parse(file); test::use_fixture(profile, fixtures, "model_scripted");
        for (auto& item : profile.data["plugins"]) if (item.at("manifest") == "dfy_fixture_model_scripted.json")
            item["config"] = {{"action", name == "action_text" ? "fixture.text" : "fixture.number"}, {"arguments", name == "action_text" ? Json{{"text", "typed"}} : name == "action_schema" ? Json{{"value", "invalid"}} : Json{{"value", 42}}}};
    }
    Json first;
    {
        InstanceRuntime host(profile, state);
        if (name == "run_cancel" || name == "run_restart") {
            const auto input = host.request("input.accept", {{"key", "cancel"}, {"text", "open"}});
            const auto id = wait([&] { return host.request("input.get", {{"id", input.at("id")}}).at("run_id"); });
            wait([&] { return host.request("run.get", {{"id", id}}).at("phase") == "model" ? Json(true) : Json{}; });
            if (name == "run_restart") {
                host.shutdown();
                InstanceRuntime recovered(profile, state);
                const auto resumed = wait([&] { const auto value = recovered.request("run.get", {{"id", id}}); return value.at("execution") == "completed" ? value : Json{}; });
                check(resumed.at("model_attempts").get<int>() >= 3, "restart discarded reserved model attempt");
                check(!observations(state, "scope_restored").empty(), "unfinished Run scope was not restored");
                wait([&] { return recovered.request("health").at("scopes").at("registrations") == 0 ? Json(true) : Json{}; }); return;
            }
            host.request("run.cancel", {{"id", id}});
            wait([&] { const auto health = host.request("health"); return health.at("scopes").at("scopes") == 0 && health.at("calls") == 0 ? Json(true) : Json{}; });
            check(host.request("health").at("scopes").at("registrations") == 0, "cancel retained scoped registrations"); return;
        }
        first = run(host, "one");
        if (name == "fallback_denied" || name == "fallback_budget" || name == "action_schema") {
            check(first.at("execution") == "failed", "kernel guard was bypassed");
            const auto code = first.at("error").at("code").get<std::string>();
            check(code.find(name == "fallback_denied" ? "MODEL_BINDING_DENIED" : name == "fallback_budget" ? "MODEL_BUDGET_EXHAUSTED" : "VALUE_TYPE_INVALID") != std::string::npos, code); return;
        }
        complete(first);
        if (name == "scope_budget") {
            size_t successes = 0, exhausted = 0;
            for (const auto& event : observations(state, "scoped_reply")) if (event.at("key") == "snapshot") {
                if (event.at("error").is_null()) ++successes;
                else if (event.at("error").at("code") == "NESTED_CALL_BUDGET_EXHAUSTED") ++exhausted;
            }
            check(successes == 1 && exhausted > 0, "scope call quota reset across parent calls"); return;
        }
        if (name.starts_with("fallback")) {
            check(first.at("model_attempts") == 2 && first.at("observation").at("binding") == "model.backup", "failure did not reach authorized backup with shared budget");
            check(first.at("error").is_null(), "successful fallback retained stale failure"); return;
        }
        if (name == "action_number" || name == "action_text") {
            check(first.at("results").at(0).at("outcome").at("status") == "succeeded", "executor envelope failed"); return;
        }
        wait([&] { return !observations(state, "scoped_reply").empty() ? Json(true) : Json{}; });
        if (name == "run" || name == "call") {
            wait([&] { return host.request("health").at("scopes").at("scopes") == 0 ? Json(true) : Json{}; });
            check(host.request("health").at("scopes").at("registrations") == 0, "registration survived owner lifetime");
            check(visible(first, "fixture.number") == (name == "run"), "call scope action remained visible after parent completion"); return;
        }
        check(visible(first, "fixture.number"), "scoped action was not published after context assembly");
        if (name == "explicit_close") {
            const auto second = run(host, "closed", "a", "close"); complete(second);
            check(!visible(second, "fixture.number"), "explicit close left a callable registration");
            wait([&] { for (const auto& event : observations(state, "scoped_reply")) if (event.value("key", std::string{}) == "after-close" && !event.at("error").is_null()) return Json(true); return Json{}; }); return;
        }
        if (name == "revoke") {
            host.request("policy.set", {{"expected_revision", host.request("health").at("policy").at("revision")}, {"resources", Json::array()}});
            wait([&] { return host.request("health").at("scopes").at("scopes") == 0 ? Json(true) : Json{}; });
            check(host.request("health").at("scopes").at("registrations") == 0, "revocation retained registrations"); return;
        }
        if (name == "release") {
            const auto second = run(host, "release", "a", "release"); complete(second);
            check(!visible(second, "fixture.number"), "released action remained visible"); return;
        }
        if (name == "close") {
            host.request("conversation.close", {{"id", first.at("conversation")}});
            check(host.request("health").at("scopes").at("scopes") == 0, "conversation close retained scope");
            const auto second = run(host, "reopen", "a", "skip"); complete(second);
            check(!visible(second, "fixture.number"), "reopened conversation revived prior generation"); return;
        }
        if (name == "conversation" || name == "channels") {
            const auto second = run(host, "foreign", name == "channels" ? "a" : "b", "skip", name == "channels" ? "channel.second" : ""); complete(second);
            check(!visible(second, "fixture.number") && second.at("request").at("context").empty(), "scoped data crossed conversations");
            wait([&] { return !observations(state, "facts").empty() ? Json(true) : Json{}; });
            for (const auto& page : observations(state, "facts")) for (const auto& fact : page.at("page").at("events")) check(fact.at("conversation_id") == first.at("conversation"), "subscription crossed conversation boundary");
        }
        if (name == "subscription_restart") wait([&] { return !observations(state, "facts").empty() ? Json(true) : Json{}; });
    }
    const auto before = observations(state, "facts");
    if (name == "restore_denied") {
        for (auto& item : profile.data["plugins"]) if (item.at("manifest") == "dfy_fixture_scopes.json") item["revision"] = "2";
        InstanceRuntime host(profile, state); const auto second = run(host, "changed", "a", "skip"); complete(second);
        check(!visible(second, "fixture.number") && host.request("health").at("scopes").at("registrations") == 0, "configuration change restored stale registrations"); return;
    }
    {
        InstanceRuntime host(profile, state);
        wait([&] { return !observations(state, "scope_restored").empty() ? Json(true) : Json{}; });
        wait([&] { for (const auto& event : observations(state, "scoped_reply")) if (event.at("key") == "stale-token" && !event.at("error").is_null()) return Json(true); return Json{}; });
        const auto second = run(host, "restored", "a", "skip"); complete(second);
        check(visible(second, "fixture.number") && !second.at("request").at("context").empty(), "declarative registration was not restored");
        if (name == "subscription_restart") {
            const auto pages = wait([&] { const auto value = observations(state, "facts"); return value.size() > before.size() ? value : Json{}; });
            check(pages.at(before.size()).at("page").at("events").at(0).at("id") == before.at(0).at("page").at("events").at(0).at("id"), "unacknowledged page was not replayed");
            check(pages.at(before.size()).at("registration_id") != before.at(0).at("registration_id"), "restart reused a stale authority handle");
        }
    }
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 4, "scope test CASE PROFILE WORK");
        const auto profile = sdk::path_from_utf8(argv[2]);
        const auto state = sdk::path_from_utf8(argv[3]) / (std::string("scope-") + argv[1] + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        scenario(argv[1], Profile::open(profile), state, profile.parent_path() / "catalog.json"); std::cout << "PASS " << argv[1] << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
