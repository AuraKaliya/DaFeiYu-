#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
#include <thread>
using namespace dfy::sdk;
class Channel : public Plugin {
    dfy::Database db_;
    Json config_;
public:
    static constexpr auto id = "dafeiyu.channel.simulator";
    static constexpr auto version = "0.2.0";
    Channel(Host host, Json config) : Plugin(host), db_(host.data_directory() / "channel.sqlite3"), config_(std::move(config)) {
        db_.verify_identity(host.instance_id(), id);
        db_.exec("CREATE TABLE IF NOT EXISTS deliveries(id TEXT PRIMARY KEY,plan TEXT NOT NULL,result TEXT NOT NULL)");
        host.provide("channel.simulator");
    }
    void start() {
        for (const auto& input : config_.value("inputs", Json::array()))
            host.input(input.at("key"), {{"text", input.at("text")}, {"actor", config_.value("actor", std::string("local-owner"))}, {"conversation", "local"}});
    }
    void on_event(const Json& event) {
        if (event.at("type") != "call") return;
        const Call call(event); const auto delivery = call.input.at("id").get<std::string>();
        if (call.method == "query_delivery") {
            auto query = db_.prepare("SELECT result FROM deliveries WHERE id=?"); query.bind(1, delivery);
            host.complete(call.id, query.step() ? Json::parse(query.column(0)) : Json{{"id", delivery}, {"status", "unknown"}});
        } else if (call.method == "deliver") {
            Json result;
            {
                dfy::Transaction transaction(db_);
                auto previous = db_.prepare("SELECT plan,result FROM deliveries WHERE id=?"); previous.bind(1, delivery);
                if (previous.step()) {
                    if (Json::parse(previous.column(0)) != call.input) throw std::runtime_error("DELIVERY_PLAN_CONFLICT");
                    result = Json::parse(previous.column(1));
                } else {
                    const auto status = config_.value("delivery_mode", std::string("sent"));
                    result = {{"id", delivery}, {"status", status}};
                    if (status == "sent") result["receipt_id"] = "simulated:" + delivery;
                    auto insert = db_.prepare("INSERT INTO deliveries VALUES(?,?,?)"); insert.bind(1, delivery).bind(2, call.input.dump()).bind(3, result.dump()).step();
                }
                transaction.commit();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.value("settlement_delay_ms", 0)));
            host.complete(call.id, result);
        } else throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
DFY_PLUGIN(Channel)
