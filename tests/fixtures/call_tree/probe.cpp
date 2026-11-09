#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
#include <chrono>
#include <map>
#include <thread>
using namespace dfy::sdk;

#if DFY_TREE_NODE == 1
class TreeNode : public Plugin {
    Json config_;
    std::map<uint64_t, int> steps_;
public:
    static constexpr auto id = "test.call-tree.root";
    static constexpr auto version = "0.5.0";
    TreeNode(Host host, Json config) : Plugin(host), config_(std::move(config)) { host.provide("tree.root"); }
    void on_event(const Json& event) {
        const auto mode = config_.value("mode", std::string("budget"));
        if (event.at("type") == "reply") {
            const auto parent = event.at("call_id").get<uint64_t>();
            if (!steps_.contains(parent)) return;
            if (!event.at("error").is_null()) {
                steps_.erase(parent); host.complete(parent, outcome({{"error", event.at("error")}}, "none", "failed")); return;
            }
            const int step = ++steps_.at(parent);
            if ((mode == "composite" || mode == "revoke_resource") && step == 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(config_.value("between_steps_ms", 0)));
                host.request(parent, "beta", "tree.leaf", "write_beta", Json::object()); return;
            }
            if (mode == "revoke_action" && step == 1) { host.request(parent, "write", "tree.leaf", "write_alpha", Json::object()); return; }
            if (mode == "budget" && step < config_.value("requests", 1)) { host.request(parent, "read-" + std::to_string(step), "tree.leaf", "inspect_alpha", Json::object()); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.value("hold_result_ms", 0)));
            steps_.erase(parent);
            host.complete(parent, outcome(event.at("result"), mode == "composite" ? "confirmed" : "none")); return;
        }
        if (event.at("type") != "call") return;
        const Call call(event);
        if (call.method == "query_operation") { host.complete(call.id, outcome(Json::object(), "unknown", "unknown")); return; }
        if (call.method != "execute") throw std::runtime_error("TREE_METHOD_INVALID");
        steps_[call.id] = 0;
        if (mode == "channel_bypass") host.request(call.id, "bypass", "channel", "deliver", Json::object());
        else if (mode == "readonly_hop") host.request(call.id, "relay", "tree.relay", "read", Json::object());
        else if (mode == "resource_escape") host.request(call.id, "relay", "tree.relay", "escape", Json::object());
        else if (mode == "composite" || mode == "revoke_resource") host.request(call.id, "alpha", "tree.leaf", "write_alpha", Json::object());
        else if (mode == "late_write") {
            host.request(call.id, "late", "tree.leaf", "write_alpha", {{"delay_ms", 400}});
            steps_.erase(call.id); host.complete(call.id, outcome({{"submitted", true}}));
        } else {
            if (mode == "deadline") std::this_thread::sleep_for(std::chrono::milliseconds(120));
            const int delay = mode == "deadline" ? 800 : mode == "parent_complete" ? 200 : mode == "revoke_action" ? 500 : 0;
            host.request(call.id, "read-0", "tree.leaf", "inspect_alpha", {{"delay_ms", delay}}, config_.value("child_timeout_ms", 10000));
            if (mode == "parent_complete") { steps_.erase(call.id); host.complete(call.id, outcome({{"submitted", true}})); }
        }
    }
};
#elif DFY_TREE_NODE == 2
class TreeNode : public Plugin {
public:
    static constexpr auto id = "test.call-tree.relay";
    static constexpr auto version = "0.5.0";
    TreeNode(Host host, const Json&) : Plugin(host) { host.provide("tree.relay"); }
    void on_event(const Json& event) {
        if (event.at("type") == "reply") {
            host.complete(event.at("call_id"), event.at("error").is_null() ? event.at("result") : Json{{"error", event.at("error")}}); return;
        }
        if (event.at("type") != "call") return;
        const Call call(event);
        host.request(call.id, "leaf", "tree.leaf", call.method == "escape" ? "inspect_beta" : "write_alpha", Json::object());
    }
};
#else
class TreeNode : public Plugin {
    dfy::Database db_;
public:
    static constexpr auto id = "test.call-tree.leaf";
    static constexpr auto version = "0.5.0";
    TreeNode(Host host, const Json&) : Plugin(host), db_(host.data_directory() / "effects.sqlite3") {
        db_.verify_identity(host.instance_id(), id);
        db_.exec("CREATE TABLE IF NOT EXISTS effects(id TEXT PRIMARY KEY, resource TEXT, plan TEXT, result TEXT)");
        host.provide("tree.leaf");
    }
    void on_event(const Json& event) {
        if (event.at("type") != "call") return;
        const Call call(event);
        if (call.method == "query_operation") {
            auto query = db_.prepare("SELECT result FROM effects WHERE id=?"); query.bind(1, call.input.at("id").get<std::string>());
            host.complete(call.id, query.step() ? Json::parse(query.column(0)) : outcome(Json::object(), "unknown", "unknown")); return;
        }
        const bool writing = call.method.starts_with("write_");
        const auto resource = call.method.ends_with("beta") ? "beta" : "alpha";
        Json result{{"resource", resource}};
        if (writing) {
            const auto operation = call.context.at("operation_id").get<std::string>();
            dfy::Transaction tx(db_);
            auto previous = db_.prepare("SELECT plan,result FROM effects WHERE id=?"); previous.bind(1, operation);
            if (previous.step()) {
                if (Json::parse(previous.column(0)) != call.input) throw std::runtime_error("TREE_WRITE_CONFLICT");
                result = Json::parse(previous.column(1));
            } else {
                result = outcome({{"resource", resource}}, "confirmed"); result["receipts"].push_back({{"id", operation}});
                auto insert = db_.prepare("INSERT INTO effects VALUES(?,?,?,?)");
                insert.bind(1, operation).bind(2, resource).bind(3, call.input.dump()).bind(4, result.dump()).step();
            }
            tx.commit();
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(call.input.value("delay_ms", 0));
        while (std::chrono::steady_clock::now() < until) {
            if (!writing && host.cancelled(call.id)) { host.fail(call.id, error("CANCELLED", "cancelled")); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        host.complete(call.id, result);
    }
};
#endif
DFY_PLUGIN(TreeNode)
