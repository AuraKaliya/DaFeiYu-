#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
using namespace dfy::sdk;
class Channel : public Plugin {
    dfy::Database db_;
public:
    static constexpr auto id = "dafeiyu.channel.simulator";
    static constexpr auto version = "0.5.0";
    Channel(Host host, const Json&) : Plugin(host), db_(host.data_directory() / "channel.sqlite3") {
        db_.verify_identity(host.instance_id(), id);
        db_.exec("CREATE TABLE IF NOT EXISTS deliveries(id TEXT PRIMARY KEY,plan TEXT NOT NULL,result TEXT NOT NULL)");
        host.provide("channel.simulator");
    }
    void on_event(const Json& event) {
        if (event.at("type") != "call") return;
        const Call call(event); const auto delivery = call.input.at("id").get<std::string>();
        if (call.method == "query_delivery") {
            auto query = db_.prepare("SELECT result FROM deliveries WHERE id=?"); query.bind(1, delivery);
            host.complete(call.id, query.step() ? Json::parse(query.column(0)) : Json{{"id", delivery}, {"status", "unknown"}});
        } else if (call.method == "deliver") {
            if (call.input.at("binding").at("channel").at("service") != call.service) throw std::runtime_error("CHANNEL_DISPATCH_MISMATCH");
            Json result;
            {
                dfy::Transaction transaction(db_);
                auto previous = db_.prepare("SELECT plan,result FROM deliveries WHERE id=?"); previous.bind(1, delivery);
                if (previous.step()) {
                    if (Json::parse(previous.column(0)) != call.input) throw std::runtime_error("DELIVERY_PLAN_CONFLICT");
                    result = Json::parse(previous.column(1));
                } else {
                    result = {{"id", delivery}, {"status", "sent"}, {"receipt_id", "simulated:" + delivery}};
                    auto insert = db_.prepare("INSERT INTO deliveries VALUES(?,?,?)"); insert.bind(1, delivery).bind(2, call.input.dump()).bind(3, result.dump()).step();
                }
                transaction.commit();
            }
            host.complete(call.id, result);
        } else throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
DFY_PLUGIN(Channel)
