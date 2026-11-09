#include "../host/control.hpp"
#include "profiles/scenario.hpp"
#include <dafeiyu/sqlite.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
using dfy::Json;
using dfy::Path;
namespace {
void check(bool condition, const std::string& reason) { if (!condition) throw std::runtime_error(reason); }
class Process {
#ifdef _WIN32
    HANDLE process_{};
#else
    pid_t process_{-1};
#endif
public:
    Process(const Path& executable, const Path& profile, const Path& state) {
#ifdef _WIN32
        std::wstring line = L"\"" + executable.wstring() + L"\" serve --profile \"" + profile.wstring() + L"\" --state \"" + state.wstring() + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION info{};
        check(CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) != 0, "CreateProcess failed");
        process_ = info.hProcess; CloseHandle(info.hThread);
#else
        const auto exe = dfy::sdk::path_text(executable), prof = dfy::sdk::path_text(profile), dir = dfy::sdk::path_text(state);
        process_ = fork(); check(process_ >= 0, "fork failed");
        if (process_ == 0) { execl(exe.c_str(), exe.c_str(), "serve", "--profile", prof.c_str(), "--state", dir.c_str(), static_cast<char*>(nullptr)); _exit(127); }
#endif
    }
    ~Process() { kill(); }
    void kill() {
#ifdef _WIN32
        if (!process_) return;
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) TerminateProcess(process_, 73);
        WaitForSingleObject(process_, 10000); CloseHandle(process_); process_ = nullptr;
#else
        if (process_ < 0) return;
        int status{};
        if (waitpid(process_, &status, WNOHANG) == 0) { ::kill(process_, SIGKILL); waitpid(process_, &status, 0); }
        process_ = -1;
#endif
    }
    void wait_exit() {
#ifdef _WIN32
        check(WaitForSingleObject(process_, 15000) == WAIT_OBJECT_0, "host did not exit");
        DWORD code{}; GetExitCodeProcess(process_, &code); check(code == 0, "host exit failure");
#else
        int status{}; check(waitpid(process_, &status, 0) == process_ && WIFEXITED(status) && WEXITSTATUS(status) == 0, "host exit failure"); process_ = -1;
#endif
    }
};
template<class Fn> Json wait(Fn fn, int timeout = 20000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (std::chrono::steady_clock::now() < end) {
        auto value = fn(); if (!value.is_null()) return value;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("integration timeout");
}
Json& config(Json& profile, const std::string& name) {
    for (auto& plugin : profile["plugins"]) if (plugin.at("manifest") == "dfy_" + name + ".json" || plugin.at("manifest") == "dfy_fixture_" + name + ".json") return plugin["config"];
    throw std::runtime_error("config missing");
}
int64_t count(const Path& path, const std::string& table) {
    if (!std::filesystem::exists(path)) return 0;
    dfy::Database db(path); auto query = db.prepare("SELECT COUNT(*) FROM " + table); query.step(); return std::stoll(query.column(0));
}
void test(const std::string& mode, const Path& executable, const Path& original, const Path& root) {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const Path directory = root / (mode + "-" + suffix); std::filesystem::create_directories(directory);
    const Path state = directory / dfy::sdk::path_from_utf8("常驻 实例"), profile_path = directory / "profile.json";
    Json profile = dfy::test::scenario(dfy::Profile::open(original), original.parent_path() / "catalog.json", "host." + mode).data;
    profile["plugin_directory"] = dfy::sdk::path_text(std::filesystem::absolute(original.parent_path() / dfy::sdk::path_from_utf8(profile.at("plugin_directory").get<std::string>())));
    profile["foundation"] = dfy::sdk::path_text(std::filesystem::absolute(original.parent_path() / dfy::sdk::path_from_utf8(profile.at("foundation").get<std::string>())));
    profile["activities"] = Json::array();
    if (mode == "call_budget") {
        profile["bindings"]["tree.relay"] = "tree.relay"; profile["bindings"]["tree.leaf"] = "tree.leaf";
        profile["allowed_actions"] = Json::array({"fixture.tree.read"});
        profile["resources"] = Json::array({"instance", "alpha", "beta"}); profile["limits"]["nested_calls"] = 1;
        config(profile, "model_scripted") = {{"action", "fixture.tree.read"}, {"arguments", Json::object()}};
        for (const auto* role : {"root", "relay", "leaf"}) profile["plugins"].push_back({
            {"manifest", "dfy_fixture_tree_" + std::string(role) + ".json"}, {"revision", "1"},
            {"config", std::string(role) == "root" ? Json{{"mode", "budget"}, {"hold_result_ms", 5000}} : Json::object()}});
    }
    if (mode == "delivery") config(profile, "channel_simulator")["settlement_delay_ms"] = 5000;
    if (mode == "job") config(profile, "action_background")["delay_ms"] = 5000;
    if (mode == "resident") config(profile, "model_scripted")["delay_ms"] = 300;
    { std::ofstream file(profile_path); file << profile.dump(2); }
    auto send = [&](const std::string& command, const Json& args = Json::object()) { return dfy::host::send(state, command, args); };
    auto online = [&] {
        wait([&] { return std::filesystem::exists(state / "control" / "endpoint.json") ? Json(true) : Json{}; });
        return send("health");
    };
    auto submit = [&](const std::string& key, const std::string& text) {
        const auto receipt = send("input.accept", {{"key", key}, {"text", text}});
        return wait([&] { auto input = send("input.get", {{"id", receipt.at("id")}}); return input.at("run_id").is_null() ? Json{} : input.at("run_id"); }).get<std::string>();
    };
    auto finish = [&](const std::string& id) {
        return wait([&] { auto run = send("run.get", {{"id", id}});
            if (run.at("execution") == "failed") throw std::runtime_error(run.at("error").dump());
            return run.at("execution") == "completed" ? run : Json{}; });
    };
    std::string id, identity;
    {
        Process host(executable, profile_path, state);
        identity = online().at("instance_id");
#ifdef _WIN32
        if (mode == "control_sharing") {
            const auto endpoint = state / "control/endpoint.json";
            const auto held = CreateFileW(endpoint.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            check(held != INVALID_HANDLE_VALUE, "fixture open with delete access");
            try { check(send("health").at("state") == "running", "published endpoint readable during rename handle lifetime"); }
            catch (...) { CloseHandle(held); throw; }
            CloseHandle(held);
            send("host.shutdown"); host.wait_exit(); return;
        }
#endif
        const auto text = mode == "action" ? "unknown:write" : mode == "job" ? "background:work" : "demo";
        id = submit("first", text);
        if (mode == "resident") {
            const auto run = finish(id); check(run.at("delivery") == "sent", "delivery not confirmed");
            auto behavior = send("health").at("behavior");
            send("behavior.set", {{"expected_revision", behavior.at("revision")}, {"mode", "resting"}});
            send("input.accept", {{"key", "queued"}, {"text", "search:after restart"}});
            send("host.shutdown"); host.wait_exit();
        } else {
            if (mode == "delivery") {
                wait([&] { return count(state / "plugins/dafeiyu.channel.simulator/channel.sqlite3", "deliveries") == 1 ? Json(true) : Json{}; });
            } else if (mode == "action") {
                wait([&] { return count(state / "plugins/dafeiyu.action.write-demo/effects.sqlite3", "effects") == 1 ? Json(true) : Json{}; });
            } else if (mode == "call_budget") {
                wait([&] { const auto page = send("events", {{"run_id", id}, {"types", Json::array({"call.budget_reserved"})}});
                    return page.at("events").size() == 1 ? Json(true) : Json{}; });
            } else {
                wait([&] { auto jobs = send("job.list").value("items", Json::array()); return !jobs.empty() && count(state / "plugins/dafeiyu.action.background/jobs.sqlite3", "work") == 1 ? Json(true) : Json{}; });
            }
            host.kill();
            // A crashed process cannot remove its endpoint. A client must wait for the next epoch.
            std::filesystem::remove(state / "control" / "endpoint.json");
        }
    }
    {
        Process host(executable, profile_path, state);
        check(online().at("instance_id") == identity, "identity changed after restart");
        if (mode == "resident") {
            const auto input = send("input.get", {{"id", "local:queued"}});
            check(input.at("run_id").is_null(), "resting input unexpectedly dispatched");
            auto behavior = send("health").at("behavior");
            send("behavior.set", {{"expected_revision", behavior.at("revision")}, {"mode", "available"}});
            const auto next = submit("queued", "search:after restart"); finish(next);
            check(send("run.list").value("items", Json::array()).size() == 2, "resident task count");
        } else if (mode == "action") {
            auto blocked = wait([&] { auto run = send("run.get", {{"id", id}}); return run.at("execution") == "waiting" ? run : Json{}; });
            check(blocked.at("model_attempts") == 1, "unknown write replayed");
            check(count(state / "plugins/dafeiyu.action.write-demo/effects.sqlite3", "effects") == 1, "duplicate effect");
        } else {
            auto run = finish(id);
            if (mode == "delivery") check(count(state / "plugins/dafeiyu.channel.simulator/channel.sqlite3", "deliveries") == 1, "delivery replayed");
            else if (mode == "call_budget") {
                check(run.at("results").at(0).at("outcome").at("data").at("error").at("code") == "NESTED_CALL_BUDGET_EXHAUSTED", "budget reset after crash");
                const auto page = send("events", {{"run_id", id}, {"types", Json::array({"call.budget_reserved"})}});
                check(page.at("events").size() == 1, "second dispatch bypassed persisted budget");
            }
            else check(send("job.list").value("items", Json::array()).size() == 1 && run.at("results").at(0).at("outcome").at("data").at("value") == 42, "job identity or result lost");
        }
        send("host.shutdown"); host.wait_exit();
    }
}
}
int main(int argc, char** argv) {
    try { check(argc == 5, "integration MODE HOST PROFILE WORK"); test(argv[1], dfy::sdk::path_from_utf8(argv[2]), dfy::sdk::path_from_utf8(argv[3]), dfy::sdk::path_from_utf8(argv[4])); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
