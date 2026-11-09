#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
using namespace dfy::sdk;
class WriteDemo : public Plugin {
    dfy::Database db_;
    bool unknown_;
public:
    static constexpr auto id = "dafeiyu.action.write-demo";
    static constexpr auto version = "0.4.0";
    WriteDemo(Host host, const Json& config) : Plugin(host), db_(host.data_directory() / "effects.sqlite3"), unknown_(config.value("unknown", true)) {
        db_.verify_identity(host.instance_id(), id);
        db_.exec("CREATE TABLE IF NOT EXISTS effects(id TEXT PRIMARY KEY,input TEXT NOT NULL)");
        host.provide("action.write_demo");
    }
    void on_event(const Json& event) {
        if (event.at("type") != "call") return;
        const Call call(event);
        if (call.method == "readiness") { host.complete(call.id, {{"available", true}}); return; }
        if (call.method == "execute") {
            const auto operation = call.context.at("operation_id").get<std::string>();
            { dfy::Transaction transaction(db_); auto insert = db_.prepare("INSERT OR IGNORE INTO effects VALUES(?,?)"); insert.bind(1, operation).bind(2, call.input.dump()).step(); transaction.commit(); }
            host.complete(call.id, outcome({{"stored", true}}, unknown_ ? "unknown" : "confirmed", unknown_ ? "unknown" : "succeeded"));
        } else if (call.method == "query_operation") host.complete(call.id, outcome(Json::object(), "unknown", "unknown"));
        else throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
DFY_PLUGIN(WriteDemo)
