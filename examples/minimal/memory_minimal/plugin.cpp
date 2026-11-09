#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
using namespace dfy::sdk;
class Memory : public Plugin {
    dfy::Database db_;
    std::string instance_;
    Json snapshot() {
        auto query = db_.prepare("SELECT body FROM revisions ORDER BY revision DESC LIMIT 1");
        if (!query.step()) throw std::runtime_error("MEMORY_MISSING");
        return Json::parse(query.column(0));
    }
public:
    static constexpr auto id = "dafeiyu.memory.minimal";
    static constexpr auto version = "0.5.0";
    Memory(Host host, const Json& config) : Plugin(host), db_(host.data_directory() / "memory.sqlite3"), instance_(host.instance_id()) {
        db_.verify_identity(instance_, id);
        db_.exec("CREATE TABLE IF NOT EXISTS revisions(revision INTEGER PRIMARY KEY,body TEXT NOT NULL)");
        Json initial{{"revision", 1}, {"instance_id", instance_}, {"persona", config.at("seed_persona")}, {"sources", Json::array({"deployment"})}};
        auto insert = db_.prepare("INSERT OR IGNORE INTO revisions VALUES(1,?)"); insert.bind(1, initial.dump()).step();
        host.provide("memory.minimal");
    }
    void on_event(const Json& event) {
        if (event.at("type") != "call") return;
        const Call call(event);
        if (call.method == "snapshot") host.complete(call.id, snapshot());
        else if (call.method == "validate") {
            auto query = db_.prepare("SELECT body FROM revisions WHERE revision=?"); query.bind(1, call.input.at("revision").dump());
            host.complete(call.id, {{"valid", query.step() && Json::parse(query.column(0)) == call.input}});
        } else if (call.method == "retrieve") {
            const auto current = snapshot();
            host.complete(call.id, {{"items", Json::array({{{"content", current.at("persona")}, {"revision", current.at("revision")}, {"source", "memory"}, {"scope", call.input.at("scope")}}})}});
        } else throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
DFY_PLUGIN(Memory)
