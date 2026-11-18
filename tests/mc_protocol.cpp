#include "world_protocol.hpp"
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
// 确认畸形或串线的快照不会交付调用者。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid MC message accepted");
}
}

// 用固定跨端消息覆盖有效、撤销、非法上限与错身份。
// 步骤：校验正常回执 → 更改来源和字段 → 验证撤销与配置边界。
int main() {
    try {
        const std::string id = "1.2";
        Json value = {{"version", 1}, {"id", id}, {"state", {
            {"generation", "12345678-1234-1234-1234-123456789abc"}, {"actor", "abcd1234-1234-1234-1234-123456789abc"},
            {"dimension", "overworld"}, {"position", {{"x", -1.5}, {"y", -60}, {"z", 0.5}}},
            {"cobblestone", 5}, {"signalAgeMs", 20}}}};
        if (Json::parse(dfy::mc::world_request(id)).at("method") != "state.read" ||
            Json::parse(dfy::mc::world_reply(value.dump(), id)) != value.at("state")) return 1;
        rejects([&] { dfy::mc::world_reply(value.dump(), "wrong"); });
        const Json bad_fields{{"generation", "old"}, {"actor", 1}, {"dimension", "\n"},
            {"position", {{"x", false}, {"y", 0}, {"z", 0}}}, {"cobblestone", -1}, {"signalAgeMs", 1.5}};
        for (const auto& [key, bad] : bad_fields.items()) {
            auto copy = value;
            copy["state"][key] = bad;
            rejects([&] { dfy::mc::world_reply(copy.dump(), id); });
        }
        value["state"] = nullptr;
        if (dfy::mc::world_reply(value.dump(), id) != "null" || dfy::mc::configured_channel(R"({"channel":"test"})") != "test") return 1;
        rejects([] { dfy::mc::configured_channel(R"({"channel":"ok","channel":"also-ok"})"); });
        rejects([] { dfy::mc::configured_channel(R"({"channel":"../outside"})"); });
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
