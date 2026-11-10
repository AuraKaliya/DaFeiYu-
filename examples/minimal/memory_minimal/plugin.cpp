#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
using namespace dfy::sdk;

class Memory : public Plugin {
    dfy::Database db_;
    std::string instance_;

    // 读取最新人格版本，缺少初始记忆时报告错误。
    Json snapshot() {
        auto query = db_.prepare("SELECT body FROM revisions ORDER BY revision DESC LIMIT 1");
        if (!query.step())
            throw std::runtime_error("MEMORY_MISSING");
        return Json::parse(query.column(0));
    }

public:
    static constexpr auto id = "dafeiyu.memory.minimal";
    static constexpr auto version = "0.5.0";

    // 初始化独立人格版本库，并登记记忆读取服务。
    // 步骤：校验数据库身份并建表 → 幂等写入初始人格 → 注册记忆服务。
    Memory(Host host, const Json& config)
        : Plugin(host), db_(host.data_directory() / "memory.sqlite3"), instance_(host.instance_id()) {
        db_.verify_identity(instance_, id);
        db_.exec("CREATE TABLE IF NOT EXISTS revisions(revision INTEGER PRIMARY KEY,body TEXT NOT NULL)");

        Json initial{{"revision", 1}, {"instance_id", instance_}, {"persona", config.at("seed_persona")},
            {"sources", Json::array({"deployment"})}};
        auto insert = db_.prepare("INSERT OR IGNORE INTO revisions VALUES(1,?)");
        insert.bind(1, initial.dump()).step();

        host.provide("memory.minimal");
    }

    // 处理人格快照、版本校验和简单记忆检索请求。
    // 步骤：筛选记忆调用 → 按方法读取或验证人格版本 → 回传快照、有效性或检索条目。
    void on_event(const Json& event) {
        if (event.at("type") != "call")
            return;
        const Call call(event);

        if (call.method == "snapshot")
            host.complete(call.id, snapshot());
        else if (call.method == "validate") {
            auto query = db_.prepare("SELECT body FROM revisions WHERE revision=?");
            query.bind(1, call.input.at("revision").dump());
            host.complete(call.id, {{"valid", query.step() && Json::parse(query.column(0)) == call.input}});
        } else if (call.method == "retrieve") {
            const auto current = snapshot();
            host.complete(call.id,
                {{"items", Json::array({{{"content", current.at("persona")}, {"revision", current.at("revision")},
                               {"source", "memory"}, {"scope", call.input.at("scope")}}})}});
        } else
            throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
DFY_PLUGIN(Memory)
