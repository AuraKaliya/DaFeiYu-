#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
#include <chrono>
using namespace dfy::sdk;
class ClockContext : public Plugin {
    dfy::Database db_;
public:
    static constexpr auto id = "dafeiyu.context.clock";
    static constexpr auto version = "0.4.0";
    ClockContext(Host host, const Json&) : Plugin(host), db_(host.data_directory() / "observations.sqlite3") {
        db_.verify_identity(host.instance_id(), id);
        db_.exec("CREATE TABLE IF NOT EXISTS seen(id TEXT PRIMARY KEY)");
        host.provide("context.clock");
    }
    void start() { host.subscribe("operations", Json::array({"operation.settled"})); }
    void on_event(const Json& event) {
        if (event.at("type") == "facts") {
            { dfy::Transaction transaction(db_);
              for (const auto& fact : event.at("page").at("events")) { auto insert = db_.prepare("INSERT OR IGNORE INTO seen VALUES(?)"); insert.bind(1, fact.at("id").get<std::string>()).step(); }
              transaction.commit(); }
            host.acknowledge("operations", event.at("page").at("cursor")); return;
        }
        if (event.at("type") != "call") return;
        const Call call(event);
        if (call.method != "collect") throw std::runtime_error("METHOD_UNSUPPORTED");
        const auto time = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        host.complete(call.id, {{"items", Json::array({{{"source", "clock"}, {"revision", 1}, {"scope", call.input.at("scope")},
            {"content", "Unix time: " + std::to_string(time)}, {"expires_at", 0}}})}});
    }
};
DFY_PLUGIN(ClockContext)
