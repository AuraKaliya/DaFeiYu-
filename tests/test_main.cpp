#include <dafeiyu/core.hpp>
#include <fstream>
#include <iostream>
#include <random>

using namespace dfy;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Fn> void rejects(Fn&& fn, std::string_view expected) {
    try { fn(); } catch (const std::exception& e) {
        check(std::string_view(e.what()).find(expected) != std::string_view::npos, e.what());
        return;
    }
    throw std::runtime_error("expected rejection");
}
size_t count(const Json& events, const char* type) {
    size_t total = 0;
    for (const auto& event : events) if (event.at("type") == type) ++total;
    return total;
}
int rows(const Path& file, const char* table) {
    Database db(file);
    auto query = db.prepare(std::string("SELECT count(*) FROM ") + table);
    check(query.step(), "count missing");
    return std::stoi(query.column(0));
}
void write(const Path& path, const Json& value) { std::ofstream out(path); out << value.dump(2); if (!out) throw std::runtime_error("fixture write failed"); }
void copy_plugins(Profile& profile, const Path& target) {
    std::filesystem::create_directories(target);
    for (const auto& entry : std::filesystem::directory_iterator(profile.plugins)) {
        if (entry.is_regular_file()) std::filesystem::copy_file(entry.path(), target / entry.path().filename());
    }
    profile.plugins = target;
}
void execute(const std::string& test, Profile profile, const Path& state) {
    if (test == "loop") {
        Runtime runtime(profile, state);
        check(runtime.plugins().size() == 4, "four native services");
        const auto run = runtime.submit("one", "hello");
        check(run.at("execution") == "completed" && run.at("delivery") == "sent" && run.at("goal") == "met", "loop settlement");
        check(run.at("results")[0].at("outcome").at("data").at("sum") == 13, "action result");
        check(run.at("memory").at("revision") == 1, "memory revision");
        const auto events = runtime.events("one");
        check(count(events, "model.request") == 2 && count(events, "operation.started") == 1, "trajectory");
        for (const auto& event : events) if (event.at("type") == "model.request")
            check(event.at("payload").at("memory") == run.at("memory"), "record actual model context");
    } else if (test == "checkpoint") {
        Json original;
        { Runtime first(profile, state); RunOptions options; options.pause_after_action = true;
          original = first.submit("one", "hello", options); check(original.at("execution") == "waiting", "paused"); }
        Runtime second(profile, state);
        const auto restored = second.resume("one");
        check(restored.at("run_id") == original.at("run_id") && restored.at("memory") == original.at("memory"), "stable recovery identity");
        check(restored.at("delivery") == "sent", "resume delivery");
        check(count(second.events("one"), "operation.started") == 1, "do not replay settled action");
    } else if (test == "dedup") {
        Runtime runtime(profile, state);
        const auto first = runtime.submit("one", "hello");
        const auto events = runtime.events("one");
        check(runtime.submit("one", "hello") == first && runtime.events("one") == events, "duplicate request is read-only");
        rejects([&] { runtime.submit("one", "changed"); }, "REQUEST_ID_CONFLICT");
        check(rows(state / "plugins/dafeiyu.channel.simulator/channel.sqlite3", "deliveries") == 1, "exactly one simulator receipt");
    } else if (test == "denied_action" || test == "invalid_arguments") {
        if (test == "denied_action") profile.data["allowed_actions"] = Json::array();
        else profile.data["plugins"][0]["config"]["arguments"]["b"] = "bad";
        Runtime runtime(profile, state);
        const auto run = runtime.submit("one", "hello");
        check(run.at("execution") == "failed", "deny invalid dispatch");
        check(run.at("error").get<std::string>().find(test == "denied_action" ? "ACTION_NOT_AUTHORIZED" : "ACTION_ARGUMENT_TYPE") != std::string::npos, "specific rejection");
        check(count(runtime.events("one"), "operation.started") == 0 && count(runtime.events("one"), "delivery.started") == 0, "no effects on rejection");
    } else if (test == "unknown_delivery") {
        profile.data["plugins"][1]["config"]["delivery_mode"] = "unknown";
        { Runtime runtime(profile, state); const auto run = runtime.submit("one", "hello");
          check(run.at("goal") == "met" && run.at("delivery") == "unknown" && run.at("execution") == "waiting", "goal and delivery independent"); }
        Runtime reopened(profile, state);
        check(reopened.resume("one").at("delivery") == "unknown", "unknown remains unknown");
        check(count(reopened.events("one"), "delivery.started") == 1, "no blind resend");
    } else if (test == "foreign_actor") {
        profile.data["plugins"][1]["config"]["actor"] = "stranger";
        Runtime runtime(profile, state);
        rejects([&] { runtime.submit("one", "hello"); }, "INGRESS_ACTOR_DENIED");
        check(runtime.status("one").is_null(), "no task for unauthorized actor");
    } else if (test == "not_sent") {
        profile.data["plugins"][1]["config"]["delivery_mode"] = "not-sent";
        Runtime runtime(profile, state);
        check(runtime.submit("one", "hello").at("delivery") == "not-sent", "known non-delivery");
        const auto run = runtime.resume("one");
        check(run.at("execution") == "waiting" && run.at("delivery") == "not-sent", "no implicit retry of confirmed failure");
        check(count(runtime.events("one"), "delivery.started") == 1, "no repeated send");
    } else if (test == "observer_failure") {
        Runtime runtime(profile, state);
        RunOptions options;
        options.observer = [](std::string_view) { throw std::runtime_error("observer unavailable"); };
        const auto run = runtime.submit("one", "hello", options);
        check(run.at("execution") == "completed" && run.at("delivery") == "sent", "observer must not veto committed facts");
        check(count(runtime.events("one"), "observer.error") > 0, "observer failure diagnosed");
    } else if (test == "cancellation") {
        Runtime runtime(profile, state);
        std::stop_source stop;
        RunOptions options; options.stop = stop.get_token();
        options.observer = [&](std::string_view event) { if (event == "model.response") stop.request_stop(); };
        check(runtime.submit("one", "hello", options).at("execution") == "cancelled", "cancel at boundary");
        check(count(runtime.events("one"), "operation.started") == 0, "cancel before effect");
        rejects([&] { runtime.resume("one"); }, "CANCELLED_RUN");
    } else if (test == "rollback" || test == "abi") {
        PluginManager manager;
        auto selection = profile.data.at("plugins");
        selection.push_back({{"manifest", test == "rollback" ? "dfy_test_fail.json" : "dfy_test_abi.json"}, {"config", Json::object()}});
        rejects([&] { manager.load(profile.plugins, state, "test-instance", selection); }, test == "rollback" ? "PLUGIN_START" : "PLUGIN_ABI");
        check(manager.service_count() == 0, "rollback all registrations");
        manager.load(profile.plugins, state, "test-instance", profile.data.at("plugins"));
        check(manager.service_count() == 4, "reload after rollback");
        manager.shutdown();
        check(manager.service_count() == 0, "shutdown unregisters");
    } else if (test == "dependency") {
        copy_plugins(profile, state / "modules");
        const auto file = profile.plugins / "dfy_model_scripted.json";
        auto manifest = read_json(file);
        manifest["requires"] = Json::array({"missing.service"}); write(file, manifest);
        PluginManager manager;
        rejects([&] { manager.load(profile.plugins, state / "data", "test", profile.data.at("plugins")); }, "DEPENDENCY_MISSING");
        manifest["requires"] = Json::array({"memory.minimal"}); write(file, manifest);
        const auto memory_file = profile.plugins / "dfy_memory_minimal.json";
        auto memory = read_json(memory_file); memory["requires"] = Json::array({"model.scripted"}); write(memory_file, memory);
        rejects([&] { manager.load(profile.plugins, state / "data", "test", profile.data.at("plugins")); }, "DEPENDENCY_CYCLE");
        check(manager.service_count() == 0, "dependency validation before activation");
    } else if (test == "identity") {
        { Runtime first(profile, state); first.submit("one", "hello"); }
        { Database memory(state / "plugins/dafeiyu.memory.minimal/memory.sqlite3"); memory.exec("UPDATE identity SET instance='foreign'"); }
        rejects([&] { Runtime second(profile, state); }, "STORAGE_IDENTITY_OR_VERSION_MISMATCH");
    } else if (test == "memory_owner") {
        Json original;
        { Runtime first(profile, state); original = first.submit("one", "hello").at("memory"); }
        profile.data["plugins"][2]["config"]["seed_persona"] = "do not replace persisted persona";
        Runtime second(profile, state);
        check(second.submit("two", "hello again").at("memory") == original, "seed never overwrites retained persona");
    } else if (test == "immutable_events") {
        { Runtime runtime(profile, state); runtime.submit("one", "hello"); }
        Database db(state / "core.sqlite3");
        rejects([&] { db.exec("DELETE FROM events"); }, "IMMUTABLE_EVENT");
        rejects([&] { db.exec("UPDATE events SET type='changed'"); }, "IMMUTABLE_EVENT");
    } else if (test == "instance_lock") {
        Runtime first(profile, state);
        rejects([&] { Runtime second(profile, state); }, "INSTANCE_IN_USE");
        check(first.submit("one", "hello").at("delivery") == "sent", "original owner survives");
    } else if (test == "policy_change") {
        { Runtime first(profile, state); RunOptions options; options.pause_after_action = true; first.submit("one", "hello", options); }
        profile.data["allowed_actions"] = Json::array();
        Runtime second(profile, state);
        rejects([&] { second.resume("one"); }, "RESUME_BINDINGS_CHANGED");
        check(count(second.events("one"), "delivery.started") == 0, "policy change cannot silently authorize resume");
    } else if (test == "budget") {
        profile.data["max_turns"] = 1;
        Runtime runtime(profile, state);
        const auto run = runtime.submit("one", "hello");
        check(run.at("execution") == "failed" && run.at("error") == "TURN_BUDGET_EXHAUSTED", "bounded model loop");
    } else throw std::runtime_error("unknown test");
}
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        const auto profile = Profile::open(sdk::path_from_utf8(argv[2]));
        const auto state = sdk::path_from_utf8(argv[3]) / (std::string(argv[1]) + "-" + std::to_string(std::random_device{}()));
        execute(argv[1], profile, state);
        std::cout << "PASS " << argv[1] << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << argv[1] << ": " << e.what() << '\n'; return 1; }
}
