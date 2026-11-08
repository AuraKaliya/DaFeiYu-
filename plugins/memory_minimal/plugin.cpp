#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
using namespace dfy::sdk;
class MinimalMemory {
    dfy::Database db_;
public:
    static constexpr auto id = "dafeiyu.memory.minimal";
    static constexpr auto version = "0.1.0";
    MinimalMemory(Host host, const Json& config) : db_(host.data_directory() / "memory.sqlite3") {
        db_.verify_identity(host.instance_id(), id);
        db_.exec("CREATE TABLE IF NOT EXISTS revisions (revision INTEGER PRIMARY KEY, snapshot TEXT NOT NULL)");
        const Json initial{{"revision", 1}, {"instance_id", host.instance_id()}, {"persona", config.at("seed_persona")},
            {"sources", Json::array({{{"id", "memory.initial"}, {"revision", 1}, {"origin", "deployment"}}})}};
        if (!initial.at("persona").is_string()) throw std::runtime_error("MEMORY_CONFIG_INVALID");
        auto insert = db_.prepare("INSERT OR IGNORE INTO revisions VALUES(1,?)");
        insert.bind(1, initial.dump()).step();
        host.provide("memory.minimal");
    }
    void start() {}
    void stop() {}
    void invoke(const Call& call, const Output& output) {
        if (call.method == "snapshot") {
            auto query = db_.prepare("SELECT snapshot FROM revisions ORDER BY revision DESC LIMIT 1");
            if (!query.step()) throw std::runtime_error("MEMORY_UNAVAILABLE");
            output.emit(Json::parse(query.column(0)));
        } else if (call.method == "validate") {
            auto query = db_.prepare("SELECT snapshot FROM revisions WHERE revision=?");
            query.bind(1, std::to_string(call.input.at("revision").get<int>()));
            output.emit({{"valid", query.step() && Json::parse(query.column(0)) == call.input}});
        } else throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
DFY_PLUGIN(MinimalMemory)
