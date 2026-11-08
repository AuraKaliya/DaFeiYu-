#pragma once
#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>

namespace dfy::examples {
class Simulator {
    Database db_;
    sdk::Json config_;
public:
    static constexpr auto id = "dafeiyu.channel.simulator";
    static constexpr auto version = "0.1.0";
    Simulator(sdk::Host host, sdk::Json config) : db_(host.data_directory() / "channel.sqlite3"), config_(std::move(config)) {
        db_.verify_identity(host.instance_id(), "simulator");
        const auto mode = config_.value("delivery_mode", std::string("sent"));
        if (mode != "sent" && mode != "unknown" && mode != "not-sent") throw std::runtime_error("SIMULATOR_MODE_INVALID");
        if (!config_.at("actor").is_string()) throw std::runtime_error("SIMULATOR_ACTOR_INVALID");
        db_.exec("CREATE TABLE IF NOT EXISTS inbox (id TEXT PRIMARY KEY, body TEXT NOT NULL);"
                 "CREATE TABLE IF NOT EXISTS deliveries (id TEXT PRIMARY KEY, plan TEXT NOT NULL, receipt TEXT NOT NULL)");
        host.provide("channel.simulator");
    }
    void start() {}
    void stop() {}
    void invoke(const sdk::Call& call, const sdk::Output& output) {
        using sdk::Json;
        if (call.method == "ingest") {
            const auto key = call.input.at("request_id").get<std::string>();
            const Json message{{"request_id", key}, {"text", call.input.at("text")}, {"actor", config_.at("actor")}, {"conversation", "simulator.private"}};
            Transaction transaction(db_);
            auto existing = db_.prepare("SELECT body FROM inbox WHERE id=?");
            existing.bind(1, key);
            if (existing.step()) {
                if (Json::parse(existing.column(0)) != message) throw std::runtime_error("INBOUND_ID_CONFLICT");
            } else {
                auto insert = db_.prepare("INSERT INTO inbox VALUES(?,?)");
                insert.bind(1, key).bind(2, message.dump()).step();
            }
            transaction.commit();
            output.emit(message);
        } else if (call.method == "deliver") {
            call.checkpoint();
            const auto key = call.input.at("id").get<std::string>();
            if (call.input.at("target") != call.context.at("scope")) throw std::runtime_error("SIMULATOR_TARGET_MISMATCH");
            Json receipt;
            {
                Transaction transaction(db_);
                auto existing = db_.prepare("SELECT plan,receipt FROM deliveries WHERE id=?");
                existing.bind(1, key);
                if (existing.step()) {
                    if (Json::parse(existing.column(0)) != call.input) throw std::runtime_error("DELIVERY_ID_CONFLICT");
                    receipt = Json::parse(existing.column(1));
                } else {
                    const auto mode = config_.value("delivery_mode", std::string("sent"));
                    receipt = {{"id", key}, {"status", mode}};
                    if (mode == "sent") receipt["receipt_id"] = "simulator:" + key;
                    auto insert = db_.prepare("INSERT INTO deliveries VALUES(?,?,?)");
                    insert.bind(1, key).bind(2, call.input.dump()).bind(3, receipt.dump()).step();
                }
                transaction.commit();
            }
            output.emit(receipt);
        } else if (call.method == "query_delivery") {
            const auto key = call.input.at("id").get<std::string>();
            auto query = db_.prepare("SELECT receipt FROM deliveries WHERE id=?");
            query.bind(1, key);
            output.emit(query.step() ? Json::parse(query.column(0)) : Json{{"id", key}, {"status", "unknown"}});
        } else throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
}
