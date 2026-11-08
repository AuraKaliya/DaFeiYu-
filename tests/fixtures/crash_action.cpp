#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
#include <cstdlib>
class CrashAction {
    dfy::Database db_;
public:
    static constexpr auto id = "test.crash-action";
    static constexpr auto version = "0.1.0";
    CrashAction(dfy::sdk::Host host, const dfy::sdk::Json&) : db_(host.data_directory() / "effects.sqlite3") {
        db_.exec("CREATE TABLE IF NOT EXISTS effects (id INTEGER PRIMARY KEY)");
        host.provide("action.add");
    }
    void start() {}
    void stop() {}
    void invoke(const dfy::sdk::Call& call, const dfy::sdk::Output& output) {
        if (call.method == "readiness") { output.emit({{"available",true}}); return; }
        if (call.method != "execute") throw std::runtime_error("METHOD_UNSUPPORTED");
        db_.exec("INSERT INTO effects DEFAULT VALUES");
        std::_Exit(73);
    }
};
DFY_PLUGIN(CrashAction)
