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

    // 初始化人格版本与变更回执库，登记记忆读取及写入服务。
    // 步骤：校验身份并创建版本和变更表 → 写入初始人格 → 注册读取及变更服务。
    Memory(Host host, const Json& config)
        : Plugin(host), db_(host.data_directory() / "memory.sqlite3"), instance_(host.instance_id()) {
        db_.verify_identity(instance_, id);
        db_.exec("CREATE TABLE IF NOT EXISTS revisions(revision INTEGER PRIMARY KEY,body TEXT NOT NULL);"
                 "CREATE TABLE IF NOT EXISTS changes(id TEXT PRIMARY KEY,plan TEXT NOT NULL,result TEXT NOT NULL)");

        Json initial{{"revision", 1}, {"instance_id", instance_}, {"persona", config.at("seed_persona")},
            {"sources", Json::array({"deployment"})}};
        auto insert = db_.prepare("INSERT OR IGNORE INTO revisions VALUES(1,?)");
        insert.bind(1, initial.dump()).step();

        host.provide("memory.minimal");
        host.provide("memory.changes");
    }

    // 处理记忆读取及带版本、幂等回执的记忆变更。
    // 步骤：按方法提供快照、校验或检索 → 变更时核对操作与版本并事务保存 → 查询或回传变更回执。
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
        } else if (call.method == "commit_change") {
            const auto change = call.context.at("operation_id").get<std::string>();
            if (change.empty())
                throw std::runtime_error("OPERATION_REQUIRED");

            Json result;
            {
                dfy::Transaction transaction(db_);

                auto previous = db_.prepare("SELECT plan,result FROM changes WHERE id=?");
                previous.bind(1, change);
                if (previous.step()) {
                    if (Json::parse(previous.column(0)) != call.input)
                        throw std::runtime_error("CHANGE_PLAN_CONFLICT");
                    result = Json::parse(previous.column(1));
                } else {
                    auto next = snapshot();
                    if (next.at("revision") != call.input.at("expected_revision"))
                        throw std::runtime_error("MEMORY_REVISION_CONFLICT");
                    next["revision"] = next.at("revision").get<int>() + 1;
                    next["persona"] = call.input.at("persona");
                    next["sources"] = Json::array({{{"operation_id", change}, {"run_id", call.context.at("run_id")}}});

                    auto insert = db_.prepare("INSERT INTO revisions VALUES(?,?)");
                    insert.bind(1, next.at("revision").dump()).bind(2, next.dump()).step();
                    result = outcome(next, "confirmed");
                    result["receipts"].push_back({{"change_id", change}});
                    auto receipt = db_.prepare("INSERT INTO changes VALUES(?,?,?)");
                    receipt.bind(1, change).bind(2, call.input.dump()).bind(3, result.dump()).step();
                }

                transaction.commit();
            }

            host.complete(call.id, result);
        } else if (call.method == "query_change") {
            auto query = db_.prepare("SELECT result FROM changes WHERE id=?");
            query.bind(1, call.input.at("id").get<std::string>());
            host.complete(
                call.id, query.step() ? Json::parse(query.column(0)) : outcome(Json::object(), "unknown", "unknown"));
        } else
            throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
DFY_PLUGIN(Memory)
