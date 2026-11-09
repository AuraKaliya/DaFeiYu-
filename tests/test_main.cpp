#include <dafeiyu/core.hpp>
#include <dafeiyu/contracts.hpp>
#include "profiles/scenario.hpp"
#include <dafeiyu/schema.hpp>
#include <dafeiyu/sqlite.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
using dfy::Json;
using dfy::Path;
using dfy::InstanceRuntime;
using dfy::Profile;
using Clock = std::chrono::steady_clock;
namespace {
void check(bool value, const std::string& reason) { if (!value) throw std::runtime_error("CHECK: " + reason); }
template<class Fn> void rejects(Fn fn, const std::string& expected) {
    try { fn(); } catch (const std::exception& error) { check(std::string(error.what()).find(expected) != std::string::npos, error.what()); return; }
    throw std::runtime_error("Expected rejection: " + expected);
}
Json read(const Path& path) { std::ifstream stream(path); return Json::parse(stream); }
void write(const Path& path, const Json& value) { std::ofstream stream(path); stream << value.dump(2); }
template<class Fn> Json wait(Fn fn, int milliseconds = 15000) {
    const auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
    Json last;
    while (Clock::now() < end) { last = fn(); if (!last.is_null()) return last; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    throw std::runtime_error("WAIT_TIMEOUT: " + last.dump());
}
std::string submit(InstanceRuntime& instance, std::string key, std::string text = "demo") {
    const auto receipt = instance.request("input.accept", {{"key", key}, {"text", text}});
    return wait([&] { auto input = instance.request("input.get", {{"id", receipt.at("id")}});
        return input.at("run_id").is_null() ? Json{} : input.at("run_id"); }).get<std::string>();
}
Json finish(InstanceRuntime& instance, const std::string& id) {
    auto result = wait([&] { auto run = instance.request("run.get", {{"id", id}});
        return run.at("execution") == "completed" || run.at("execution") == "failed" || run.at("execution") == "cancelled" ? run : Json{}; });
    check(result.at("execution") == "completed", result.value("error", Json{}).dump()); return result;
}
Json phase(InstanceRuntime& instance, const std::string& id, const std::string& name) {
    return wait([&] { auto run = instance.request("run.get", {{"id", id}}); return run.at("phase") == name ? run : Json{}; });
}
Json& config(Profile& profile, const std::string& name) {
    const auto manifest = dfy::test::manifest_name(profile, name);
    for (auto& item : profile.data.at("plugins")) if (item.at("manifest") == manifest) return item["config"];
    throw std::runtime_error("Plugin config missing");
}
Json all_events(InstanceRuntime& instance, const std::string& run = {}) {
    Json result = Json::array(); uint64_t cursor = 0;
    for (;;) {
        const auto page = instance.request("events", {{"after", cursor}, {"limit", 200}, {"run_id", run}});
        for (const auto& event : page.at("events")) result.push_back(event);
        const auto next = page.at("cursor").get<uint64_t>(); if (next == cursor) break; cursor = next;
    }
    return result;
}
void add_probe(Profile& profile, Json config_value = Json::object(), bool required = true) {
    profile.data["plugins"].push_back({{"manifest", "dfy_test_probe.json"}, {"revision", "1"}, {"required", required}, {"config", config_value}});
    profile.data["allowed_actions"].push_back("fixture.action");
    config(profile, "model_scripted") = {{"action", "fixture.action"}, {"arguments", Json::object()}};
}
void run_case(const std::string& name, Profile profile, const Path& state, const Path& work) {
    profile.data["activities"] = Json::array();
    if (name == "loop") {
        InstanceRuntime instance(profile, state);
        const auto first = finish(instance, submit(instance, "one"));
        const auto second = finish(instance, submit(instance, "two", "another deterministic input"));
        check(first.at("results").at(0).at("outcome").at("data").at("sum") == 13, "addition");
        check(second.at("results").at(0).at("outcome").at("data").at("sum") == 13, "second minimal task");
        check(instance.request("health").at("state") == "running", "resident after tasks");
    } else if (name == "dedup") {
        InstanceRuntime instance(profile, state);
        const auto one = instance.request("input.accept", {{"key", "same"}, {"text", "demo"}});
        const auto two = instance.request("input.accept", {{"key", "same"}, {"text", "demo"}});
        check(one.at("id") == two.at("id"), "same input identity");
        rejects([&] { instance.request("input.accept", {{"key", "same"}, {"text", "different"}}); }, "INPUT_KEY_CONFLICT");
        (void)finish(instance, submit(instance, "same"));
        check(instance.request("run.list").value("items", Json::array()).size() == 1, "one run");
    } else if (name == "restart_inbox" || name == "mode_expiry") {
        std::string identity;
        {
            InstanceRuntime instance(profile, state); identity = instance.request("health").at("instance_id");
            const auto until = name == "mode_expiry" ? std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 300 : 0;
            instance.request("behavior.set", {{"expected_revision", 1}, {"mode", "resting"}, {"effective_until", until}});
            instance.request("input.accept", {{"key", "queued"}, {"text", "demo"}});
        }
        InstanceRuntime instance(profile, state);
        check(instance.request("health").at("instance_id") == identity, "stable identity");
        if (name == "restart_inbox") instance.request("behavior.set", {{"expected_revision", 2}, {"mode", "available"}});
        (void)finish(instance, submit(instance, "queued"));
    } else if (name == "review") {
        config(profile, "strategy_default")["verify"] = true;
        InstanceRuntime instance(profile, state);
        auto result = finish(instance, submit(instance, "review"));
        check(result.at("model_attempts") == 3 && result.at("strategy_state").at("reviewed") == true, "review asks another model step");
    } else if (name == "provider_swap") {
        profile.data["bindings"]["search"] = "search.secondary";
        InstanceRuntime instance(profile, state);
        auto result = finish(instance, submit(instance, "search", "search:query"));
        check(result.at("results").at(0).at("outcome").at("data").at("provider") == "secondary", "provider substitution");
    } else if (name == "nested_memory") {
        InstanceRuntime instance(profile, state);
        auto result = finish(instance, submit(instance, "memory", "remember:new persona"));
        check(result.at("results").at(0).at("outcome").at("data").at("revision") == 2, "memory changed");
        check(result.at("persona").at("revision") == 1, "run persona binding stable");
        auto next = finish(instance, submit(instance, "next")); check(next.at("persona").at("revision") == 2, "new run adopts new persona");
        auto operations = instance.request("operation.list").value("items", Json::array());
        bool child = false; for (const auto& operation : operations) if (operation.at("kind") == "service" && operation.at("parent") != "") child = true;
        check(child, "nested write has child operation");
    } else if (name == "unknown_effect") {
        InstanceRuntime instance(profile, state);
        const auto id = submit(instance, "write", "unknown:one");
        auto blocked = wait([&] { auto run = instance.request("run.get", {{"id", id}}); return run.at("wait_reason").is_object() && run.at("wait_reason").value("type", std::string{}) == "effects" ? run : Json{}; });
        check(blocked.at("model_attempts") == 1, "unknown stops model continuation");
        const auto second = submit(instance, "second", "unknown:two");
        wait([&] { auto run = instance.request("run.get", {{"id", second}}); return !run.at("error").is_null() && run.at("execution") == "waiting" ? run : Json{}; });
        check(instance.request("operation.list").value("items", Json::array()).size() == 1, "new run cannot bypass resource restriction");
        auto operation = instance.request("operation.list").value("items", Json::array()).at(0);
        instance.request("operation.reconcile", {{"id", operation.at("id")}, {"expected_revision", operation.at("revision")},
            {"reason", "fixture inspected"}, {"evidence", "stored local record"}, {"result", dfy::sdk::outcome({{"stored", true}}, "confirmed")}});
        (void)finish(instance, id);
    } else if (name == "revocation" || name == "cancel" || name == "steer" || name == "responsive") {
        config(profile, "model_scripted")["delay_ms"] = 600;
        InstanceRuntime instance(profile, state);
        auto id = submit(instance, "slow"); phase(instance, id, "model");
        if (name == "responsive") {
            const auto before = Clock::now(); auto health = instance.request("health");
            check(Clock::now() - before < std::chrono::milliseconds(500), "model does not block query");
            check(health.at("calls").get<int>() > 0, "model outstanding");
            (void)finish(instance, id);
        } else if (name == "cancel") {
            auto result = instance.request("run.cancel", {{"id", id}}); check(result.at("execution") == "cancelled", "cancel terminal");
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); check(instance.request("operation.list").value("items", Json::array()).empty(), "cancel before action");
        } else if (name == "steer") {
            instance.request("run.steer", {{"id", id}, {"key", "followup"}, {"text", "search:steered"}});
            auto result = finish(instance, id); check(result.at("intent_revision") == 2, "intent revised");
            check(result.at("results").size() == 1 && result.at("results").at(0).at("action") == "action.search", "stale candidate ignored");
        } else {
            auto policy = instance.request("health").at("policy");
            instance.request("policy.set", {{"expected_revision", policy.at("revision")}, {"actions", Json::array()}});
            auto result = wait([&] { auto run = instance.request("run.get", {{"id", id}}); return run.at("execution") == "failed" ? run : Json{}; });
            check(result.at("error").at("code") == "ACTION_DENIED", "revocation checked at dispatch");
            check(instance.request("operation.list").value("items", Json::array()).empty(), "no operation started");
        }
    } else if (name == "deadline") {
        config(profile, "model_scripted")["delay_ms"] = 1000; profile.data["limits"]["call_timeout_ms"] = 100;
        InstanceRuntime instance(profile, state); auto id = submit(instance, "timeout");
        auto result = wait([&] { auto run = instance.request("run.get", {{"id", id}}); return !run.at("error").is_null() && run.at("execution") == "waiting" ? run : Json{}; });
        check(result.at("execution") == "waiting" && result.at("error").at("category") == "deadline", "deadline distinct from cancellation");
    } else if (name == "budget") {
        profile.data["limits"]["model_attempts"] = 1;
        InstanceRuntime instance(profile, state); auto id = submit(instance, "budget");
        auto result = wait([&] { auto run = instance.request("run.get", {{"id", id}}); return run.at("execution") == "failed" ? run : Json{}; });
        check(result.at("error").at("code") == "MODEL_BUDGET_EXHAUSTED", "budget enforced");
    } else if (name == "job" || name == "job_early") {
        config(profile, "action_background")["delay_ms"] = name == "job_early" ? 0 : 10000;
        InstanceRuntime instance(profile, state); auto id = submit(instance, "background", "background:work");
        if (name == "job") {
            wait([&] { auto jobs = instance.request("job.list").value("items", Json::array()); return jobs.empty() ? Json{} : jobs; });
            auto next = finish(instance, submit(instance, "interactive"));
            check(next.at("execution") == "completed", "job yields decision slot");
            instance.request("run.cancel", {{"id", id}});
            wait([&] { auto job = instance.request("job.list").value("items", Json::array()).at(0); return job.at("state") == "completed" ? job : Json{}; });
        } else {
            auto result = finish(instance, id); check(result.at("results").at(0).at("outcome").at("data").at("value") == 42, "early job result");
        }
    } else if (name == "activity") {
        profile.data["activities"] = Json::array({{{"id", "reflection"}, {"binding", "reflection"}, {"interval_ms", 100}, {"enabled", false},
            {"allowed_modes", Json::array({"available"})}, {"actions", Json::array({"action.remember"})}, {"delivery_policy", "none"}, {"conversation", "internal"}, {"misfire", "coalesce"}}});
        InstanceRuntime instance(profile, state);
        check(instance.request("run.list").value("items", Json::array()).empty(), "paused activity does not infer");
        instance.request("activity.set", {{"id", "reflection"}, {"enabled", true}, {"expected_revision", 1}});
        auto result = wait([&] { for (const auto& run : instance.request("run.list").value("items", Json::array())) if (run.at("activity") == "reflection" && run.at("execution") == "completed") return run; return Json{}; });
        instance.request("activity.set", {{"id", "reflection"}, {"enabled", false}, {"expected_revision", 2}});
        check(result.at("delivery") == "none", "internal activity does not deliver");
        wait([&] { for (const auto& event : all_events(instance, result.at("id"))) if (event.at("type") == "activity.settled") {
            check(event.at("payload").value("acknowledged", false) && event.at("payload").at("view_version") == 3, "activity did not receive public RunView"); return Json(true);
        } return Json{}; });
        (void)finish(instance, submit(instance, "user"));
    } else if (name == "duplicate_terminal" || name == "nested_denied" || name == "forged_scope") {
        add_probe(profile, {{"mode", name == "nested_denied" ? "nested" : name == "forged_scope" ? "forge" : "duplicate"}});
        InstanceRuntime instance(profile, state); auto result = finish(instance, submit(instance, "probe"));
        const auto outcome = result.at("results").at(0).at("outcome");
        if (name == "nested_denied") check(outcome.at("data").at("error").at("code") == "NESTED_EFFECT_DENIED", "readonly parent cannot write");
        if (name == "forged_scope") check(outcome.at("data").at("rejected") == true, "forged handle rejected");
        if (name == "duplicate_terminal") {
            int count = 0; for (const auto& event : all_events(instance, result.at("id"))) if (event.at("type") == "operation.settled") ++count;
            check(count == 2, "one action and one delivery settlement");
        }
    } else if (name == "startup_rollback" || name == "degraded") {
        add_probe(profile, {{"fail_start", true}}, name == "startup_rollback");
        if (name == "startup_rollback") {
            rejects([&] { InstanceRuntime instance(profile, state); }, "PLUGIN_START");
            profile.data["plugins"].erase(profile.data["plugins"].end() - 1); profile.data["allowed_actions"].erase(profile.data["allowed_actions"].end() - 1);
            config(profile, "model_scripted") = {{"action", "action.add"}, {"arguments", {{"a", 6}, {"b", 7}}}};
            InstanceRuntime instance(profile, state); (void)finish(instance, submit(instance, "after-failure"));
        } else {
            profile.data["allowed_actions"].erase(profile.data["allowed_actions"].end() - 1);
            config(profile, "model_scripted") = {{"action", "action.add"}, {"arguments", {{"a", 6}, {"b", 7}}}};
            InstanceRuntime instance(profile, state); (void)finish(instance, submit(instance, "healthy"));
            const auto catalog = instance.request("plugins");
            bool failed = false; for (const auto& plugin : catalog.at("plugins")) if (plugin.at("id") == "test.probe" && plugin.at("status") == "failed") failed = true;
            check(failed, "optional failure visible");
        }
    } else if (name == "schema") {
        auto schema = read(profile.plugins / dfy::test::manifest_name(profile, "action_search")).at("actions").at(0).at("input");
        dfy::sdk::validate_schema(schema);
        dfy::sdk::validate_value(schema, {{"query", "q"}, {"options", {{"limit", 2}}}});
        rejects([&] { dfy::sdk::validate_value(schema, {{"query", "q"}, {"options", {{"limit", "two"}}}}); }, "VALUE_TYPE");
        schema["oneOf"] = Json::array(); rejects([&] { dfy::sdk::validate_schema(schema); }, "SCHEMA_KEYWORD");
    } else if (name == "contract_conflict" || name == "abi_reject" || name == "dependency_cycle") {
        const auto selected = name == "contract_conflict" ? "search_secondary" : "action_add";
        const auto original_manifest = dfy::test::manifest_name(profile, selected);
        auto manifest = read(profile.plugins / original_manifest);
        if (name == "contract_conflict") manifest["services"][0]["methods"]["query"]["resource"] = "different";
        if (name == "abi_reject") manifest["abi_version"] = 1;
        if (name == "dependency_cycle") {
            profile.data["bindings"]["self"] = "action.add";
            manifest["requires"] = Json::array({{{"binding", "self"}, {"contract", "dafeiyu.action-executor"}, {"version", 1}}});
        }
        const auto file = profile.plugins / ("test-" + state.filename().string() + ".json"); write(file, manifest);
        for (auto& plugin : profile.data["plugins"]) if (plugin.at("manifest") == original_manifest) plugin["manifest"] = file.filename().string();
        rejects([&] { InstanceRuntime instance(profile, state); }, name == "contract_conflict" ? "CONTRACT_CONFLICT" : name == "abi_reject" ? "ABI_UNSUPPORTED" : "DEPENDENCY_CYCLE");
        std::filesystem::remove(file);
    } else if (name == "instance_lock") {
        InstanceRuntime instance(profile, state);
        rejects([&] { InstanceRuntime duplicate(profile, state); }, "INSTANCE_LOCK");
    } else if (name == "storage_version" || name == "storage_v2" || name == "storage_v3") {
        const auto version = name == "storage_v3" ? "3" : name == "storage_v2" ? "2" : "1";
        { dfy::Database db(state / "core.sqlite3"); db.exec(std::string("CREATE TABLE identity(id INTEGER PRIMARY KEY,instance TEXT,component TEXT,version INTEGER);INSERT INTO identity VALUES(1,'old','core',") + version + ")"); }
        rejects([&] { InstanceRuntime instance(profile, state); }, "VERSION_MISMATCH");
        dfy::Database db(state / "core.sqlite3"); auto query = db.prepare("SELECT version FROM identity"); query.step();
        check(query.column(0) == version, "legacy storage was modified");
    } else if (name == "behavior_plugin") {
        config(profile, "model_scripted") = {{"action", "action.behavior"}, {"arguments", {{"expected_revision", 1}, {"mode", "focused"}}}};
        InstanceRuntime instance(profile, state); (void)finish(instance, submit(instance, "behavior"));
        check(instance.request("health").at("behavior").at("mode") == "focused", "plugin uses controlled command");
    } else if (name == "steer_delivery") {
        config(profile, "channel_simulator")["settlement_delay_ms"] = 1000;
        InstanceRuntime instance(profile, state); auto id = submit(instance, "initial");
        phase(instance, id, "delivery");
        instance.request("run.steer", {{"id", id}, {"key", "updated"}, {"text", "search:new intent"}});
        auto result = finish(instance, id);
        check(result.at("intent_revision") == 2 && result.at("results").back().at("action") == "action.search", "old delivery cannot finish new intent");
    } else if (name == "rejected_steer") {
        InstanceRuntime instance(profile, state); const auto id = submit(instance, "finished"); (void)finish(instance, id);
        const auto receipt = instance.request("run.steer", {{"id", id}, {"key", "late"}, {"text", "new intent"}});
        wait([&] { auto input = instance.request("input.get", {{"id", receipt.at("id")}}); return input.at("state") == "rejected" ? input : Json{}; });
        check(instance.request("health").at("state") == "running", "rejected input does not stop host");
    } else if (name == "channel_input") {
        config(profile, "channel_simulator")["inputs"] = Json::array({{{"key", "platform-message"}, {"text", "demo"}}});
        InstanceRuntime instance(profile, state);
        const auto input_id = wait([&] { for (const auto& event : all_events(instance))
            if (event.at("type") == "input.accepted" && event.at("payload").at("key") == "platform-message") return event.at("payload").at("id"); return Json{}; });
        const auto id = wait([&] { auto input = instance.request("input.get", {{"id", input_id}}); return !input.at("run_id").is_null() ? input.at("run_id") : Json{}; });
        (void)finish(instance, id);
    } else if (name == "command_receipt") {
        InstanceRuntime instance(profile, state);
        Json input{{"expected_revision", 1}, {"mode", "resting"}, {"_command_id", "same-command"}};
        auto first = instance.request("behavior.set", input); auto second = instance.request("behavior.set", input);
        check(first == second && second.at("revision") == 2, "command durable dedup");
        input["mode"] = "available"; rejects([&] { instance.request("behavior.set", input); }, "COMMAND_ID_CONFLICT");
    } else if (name == "binding_change" || name == "config_change" || name == "unrelated_config") {
        std::string id;
        { InstanceRuntime instance(profile, state); id = submit(instance, "unknown", "unknown:write");
          wait([&] { auto run = instance.request("run.get", {{"id", id}}); return run.at("wait_reason").is_object() && run.at("wait_reason").value("type", std::string{}) == "effects" ? run : Json{}; }); }
        if (name == "binding_change") {
            for (auto& plugin : profile.data["plugins"]) if (plugin.at("manifest") == dfy::test::manifest_name(profile, "action_write_demo")) plugin["revision"] = "2";
        } else if (name == "config_change") config(profile, "action_write_demo")["unknown"] = false;
        else config(profile, "search_secondary")["label"] = "unused changed provider";
        InstanceRuntime instance(profile, state);
        if (name == "unrelated_config") instance.request("run.resume", {{"id", id}});
        else rejects([&] { instance.request("run.resume", {{"id", id}}); }, "BINDING_CHANGED");
    } else if (name == "minimal") {
        InstanceRuntime instance(profile, state);
        check(instance.request("plugins").at("plugins").size() == 5, "minimal has five roles");
        check(instance.request("plugins").at("actions").size() == 1, "one example action");
        check(instance.request("activity.list").empty() && instance.request("job.list").value("items", Json::array()).empty(), "no automatic activities or jobs");
        check(instance.request("run.list").value("items", Json::array()).empty(), "no inference without input");
        const auto result = finish(instance, submit(instance, "minimal"));
        check(result.at("delivery") == "sent", "minimal delivery");
        check(instance.request("health").at("policy").at("effects") == Json::array({"read"}), "read-only action grants");
        for (const auto& service : result.at("bindings").at("services"))
            if (service.at("contract") == "dafeiyu.memory") check(!service.at("methods").contains("commit_change"), "snapshot contract has no write method");
    } else if (name == "role_mismatch") {
        profile.data["bindings"]["strategy"] = "model.scripted";
        rejects([&] { InstanceRuntime instance(profile, state); }, "BINDING_CONTRACT_MISMATCH");
    } else if (name == "method_mismatch" || name == "contract_version") {
        const auto original = dfy::test::manifest_name(profile, "model_scripted");
        auto manifest = read(profile.plugins / original);
        if (name == "method_mismatch") manifest["services"][0]["methods"] = Json::object();
        else manifest["services"][0]["version"] = 999;
        const auto file = profile.plugins / ("test-" + state.filename().string() + ".json"); write(file, manifest);
        for (auto& plugin : profile.data["plugins"]) if (plugin.at("manifest") == original) plugin["manifest"] = file.filename().string();
        rejects([&] { InstanceRuntime instance(profile, state); }, name == "method_mismatch" ? "BUILTIN_CONTRACT_METHODS" : "BUILTIN_CONTRACT_VERSION");
        std::filesystem::remove(file);
    } else if (name == "run_view") {
        InstanceRuntime instance(profile, state); const auto id = submit(instance, "view"); (void)finish(instance, id);
        int views = 0;
        for (const auto& event : all_events(instance, id)) if (event.at("type") == "strategy.request") {
            const auto& view = event.at("payload").at("run");
            dfy::sdk::validate_value(dfy::sdk::contracts::run_view_schema(), view);
            for (const auto* field : {"bindings", "grants", "limits", "request", "foundation", "strategy_state"})
                check(!view.contains(field), std::string("private storage leaked: ") + field);
            check(dfy::sdk::RunView::from_json(view).to_json() == view, "public view round trip"); ++views;
        }
        check(views > 0, "strategy received versioned views");
    } else if (name == "context_subscription") {
        InstanceRuntime instance(profile, state); const auto result = finish(instance, submit(instance, "context"));
        check(!result.at("request").at("context").empty(), "context fixture contributed material");
        wait([&] {
            dfy::Database db(state / "plugins/dafeiyu.context.clock/observations.sqlite3");
            auto count = db.prepare("SELECT COUNT(*) FROM seen"); count.step();
            return std::stoi(count.column(0)) >= 2 ? Json(true) : Json{};
        });
    } else throw std::runtime_error("Unknown case " + name);
    (void)work;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("test CASE PROFILE WORK");
        const Path work = dfy::sdk::path_from_utf8(argv[3]);
        const auto suffix = std::to_string(Clock::now().time_since_epoch().count());
        const Path state = work / (std::string(argv[1]) + "-" + suffix);
        std::filesystem::create_directories(state);
        const auto profile_path = dfy::sdk::path_from_utf8(argv[2]);
        run_case(argv[1], dfy::test::scenario(Profile::open(profile_path), profile_path.parent_path() / "catalog.json", argv[1]), state, work);
        std::cout << "PASS " << argv[1] << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
