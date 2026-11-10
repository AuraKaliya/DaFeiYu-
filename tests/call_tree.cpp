#include <dafeiyu/core.hpp>
#include <dafeiyu/sqlite.hpp>
#include <chrono>
#include <iostream>
#include <fstream>
#include <thread>
using namespace dfy;

namespace {
// 在测试条件不成立时抛出携带失败原因的异常。
void check(bool condition, const std::string& reason) {
    if (!condition)
        throw std::runtime_error(reason);
}

// 轮询测试条件直到取得有效结果，超时则报告失败。
// 步骤：计算截止时间 → 重复执行条件并短暂等待 → 返回有效结果或报告超时。
template <class Fn> Json wait(Fn fn) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);

    while (std::chrono::steady_clock::now() < end) {
        auto value = fn();
        if (!value.is_null())
            return value;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    throw std::runtime_error("CALL_TREE_TIMEOUT");
}

// 分页收集指定运行任务的调用事实，供调用树约束断言使用。
Json events(InstanceRuntime& host, const std::string& id) {
    Json result = Json::array();
    uint64_t cursor = 0;
    for (;;) {
        const auto page = host.request("events", {{"run_id", id}, {"after", cursor}, {"limit", 200}});
        for (const auto& event : page.at("events"))
            result.push_back(event);
        const auto next = page.at("cursor").get<uint64_t>();
        if (next == cursor)
            return result;
        cursor = next;
    }
}

// 验证调用树的权限收缩、共享预算、期限与迟到副作用处理。
// 步骤：装配节点和委托配置 → 提交任务并按场景撤销或终止 → 收集调用事实及副作用证据 → 核对树级约束。
void execute(const std::string& name, Profile profile, const Path& state) {
    const bool writing = name == "readonly_hop" || name == "composite" || name == "late_write" ||
                         name == "revoke_action" || name == "revoke_resource" || name == "delegate_write_escape" ||
                         name == "channel_bypass";
    profile.data["bindings"]["tree.relay"] = "tree.relay";
    profile.data["bindings"]["tree.leaf"] = "tree.leaf";

    const std::string action = writing ? "fixture.tree.write" : "fixture.tree.read";
    profile.data["allowed_actions"] = Json::array({action});
    profile.data["allowed_effects"] = Json::array({"read", "local-write", "external-write"});
    profile.data["resources"] = Json::array({"instance", "alpha", "beta"});
    for (auto& plugin : profile.data["plugins"])
        if (plugin.at("manifest") == "dfy_model_scripted.json")
            plugin["config"] = {{"action", action}, {"arguments", Json::object()}};
    Json config{{"mode", name}};
    if (name == "delegate_write_escape")
        config["mode"] = "composite";
    if (name == "budget") {
        config["requests"] = 3;
        profile.data["limits"]["nested_calls"] = 2;
    }
    if (name == "deadline")
        profile.data["limits"]["call_timeout_ms"] = 500;
    if (name == "revoke_resource")
        config["between_steps_ms"] = 500;
    for (const auto* role : {"root", "relay", "leaf"})
        profile.data["plugins"].push_back({{"manifest", "dfy_fixture_tree_" + std::string(role) + ".json"},
            {"revision", "1"}, {"config", std::string(role) == "root" ? config : Json::object()}});
    if (name == "delegate_write_escape" || name == "channel_bypass") {
        std::ifstream source(profile.plugins / "dfy_fixture_tree_root.json");
        auto manifest = Json::parse(source);
        const auto required =
            name == "delegate_write_escape" ? Json::array({"alpha"}) : Json::array({"alpha", "beta", "channel"});
        manifest["actions"][1]["resources"] = required;
        if (name == "channel_bypass") {
            profile.data["resources"].push_back("channel");
            manifest["requires"].push_back({{"binding", "channel"}, {"contract", "dafeiyu.channel"}, {"version", 3}});
            manifest["actions"][1]["delegates"]["resources"] = required;
        }
        const auto filename = "tree-" + state.filename().string() + ".json";
        {
            std::ofstream file(profile.plugins / filename);
            file << manifest.dump(2);
        }
        for (auto& plugin : profile.data["plugins"])
            if (plugin.at("manifest") == "dfy_fixture_tree_root.json")
                plugin["manifest"] = filename;
    }
    InstanceRuntime host(profile, state);
    const auto input = host.request("input.accept", {{"key", "tree"}, {"text", "call-tree scenario"}});
    const auto id =
        wait([&] { return host.request("input.get", {{"id", input.at("id")}}).at("run_id"); }).get<std::string>();
    if (name == "revoke_action") {
        wait([&] {
            for (const auto& event : events(host, id))
                if (event.at("type") == "call.started" &&

                    event.at("payload").at("service") == "tree.leaf")
                    return Json(true);
            return Json{};
        });
        host.request("policy.set",
            {{"expected_revision", host.request("health").at("policy").at("revision")}, {"actions", Json::array()}});
    } else if (name == "revoke_resource") {
        wait([&] {
            for (const auto& op : host.request("operation.list").value("items", Json::array()))
                if (op.at("kind") == "service" && op.at("state") == "settled")
                    return Json(true);
            return Json{};
        });
        host.request("policy.set", {{"expected_revision", host.request("health").at("policy").at("revision")},
                                       {"resources", Json::array({"instance", "alpha"})}});
    }
    Json run;
    if (name == "late_write") {
        run = wait([&] {
            const auto value = host.request("run.get", {{"id", id}});
            return value.at("wait_reason").is_object() &&
                           value.at("wait_reason").value("type", std::string{}) == "effects"
                       ? value
                       : Json{};
        });
    } else
        run = wait([&] {
            const auto value = host.request("run.get", {{"id", id}});
            if (value.at("execution") == "failed")
                throw std::runtime_error(value.at("error").dump());
            return value.at("execution") == "completed" ? value : Json{};
        });

    wait([&] { return host.request("health").at("calls") == 0 ? Json(true) : Json{}; });
    const auto trajectory = events(host, id);
    if (name == "channel_bypass") {
        check(run.at("results")[0].at("outcome").at("data").at("error").at("code") ==
                  "CHANNEL_DELIVERY_COORDINATOR_REQUIRED",
            "plugin bypassed delivery ownership");
    } else if (name == "revoke_action" || name == "revoke_resource" || name == "delegate_write_escape") {
        const auto outcome = run.at("results")[0].at("outcome");
        const auto expected = name == "revoke_action"           ? "NESTED_ACTION_REVOKED"
                              : name == "delegate_write_escape" ? "NESTED_RESOURCE_DENIED"
                                                                : "RESOURCE_DENIED";
        check(outcome.at("data").at("error").at("code") == expected, outcome.dump());
        check(outcome.at("effects") == (name == "revoke_action" ? "none" : "confirmed"),
            "prior confirmed effect was lost");
        int written = 0;
        for (const auto& op : host.request("operation.list").value("items", Json::array()))
            if (op.at("kind") == "service")
                ++written;
        check(written == (name == "revoke_action" ? 0 : 1), "revoked write was dispatched");
    } else if (name == "readonly_hop" || name == "resource_escape") {
        const auto error = run.at("results")[0].at("outcome").at("data").at("error").at("code");
        check(error == (name == "readonly_hop" ? "NESTED_EFFECT_DENIED" : "NESTED_RESOURCE_DENIED"), error.dump());
    } else if (name == "composite") {
        int confirmed = 0;
        for (const auto& op : host.request("operation.list").value("items", Json::array()))
            if (op.at("kind") == "service" && op.at("result").at("effects") == "confirmed")
                ++confirmed;
        check(confirmed == 2, "composite resources did not both settle");
    } else if (name == "budget") {
        check(run.at("results")[0].at("outcome").at("data").at("error").at("code") == "NESTED_CALL_BUDGET_EXHAUSTED",
            "shared budget not enforced");
        int started = 0;
        for (const auto& e : trajectory)
            if (e.at("type") == "call.started" && e.at("payload").at("parent_call_id") != 0)
                ++started;
        check(started == 2, "child calls did not share one budget");
    } else if (name == "deadline" || name == "parent_complete") {
        std::map<uint64_t, int64_t> deadlines;
        bool ended = false, child = false;
        for (const auto& event : trajectory) {
            const auto& data = event.at("payload");
            if (event.at("type") == "call.started") {
                const auto call = data.at("call_id").get<uint64_t>(),
                           parent = data.at("parent_call_id").get<uint64_t>();
                deadlines[call] = data.at("deadline_steady_ns");
                if (parent) {
                    child = true;
                    check(deadlines.at(call) <= deadlines.at(parent), "child extended deadline");
                }
            }
            if (event.at("type") == "call.logical_ended" && data.at("parent_call_id") != 0 &&
                data.at("error").at("code") == (name == "deadline" ? "DEADLINE_EXCEEDED" : "PARENT_CALL_ENDED"))
                ended = true;
        }
        check(child && ended, "child lifetime was not bounded by parent");
    } else if (name == "late_write") {
        bool confirmed = false, late = false;
        for (const auto& op : host.request("operation.list").value("items", Json::array()))
            if (op.at("kind") == "service")
                confirmed = op.at("state") == "settled" && op.at("result").at("effects") == "confirmed";
        for (const auto& e : trajectory)
            if (e.at("type") == "call.late" && e.at("payload").at("method") == "write_alpha")
                late = true;
        check(confirmed && late, "late write receipt was lost");
        check(host.request("run.get", {{"id", id}}).at("execution") == "waiting",
            "late receipt resumed business implicitly");
    } else
        throw std::runtime_error("UNKNOWN_CALL_TREE_CASE");
}
}

// 解析调用树测试参数，并在独立状态目录中执行场景。
int main(int argc, char** argv) {
    try {
        if (argc != 4)
            return 2;
        const auto state =
            sdk::path_from_utf8(argv[3]) /
            (std::string(argv[1]) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        execute(argv[1], Profile::open(sdk::path_from_utf8(argv[2])), state);
        std::cout << "PASS " << argv[1] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
