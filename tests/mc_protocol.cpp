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

// 用固定读数验证私有收件窗口及查看行动的目标、序号和退出状态。
// 步骤：核对查询帧 → 注入收件断档或替换来源 → 验证到达和非法终态。
void private_protocol() {
    const std::string generation = "12345678-1234-1234-1234-123456789abc", key(64, 'a');
    const auto args = Json{{"generation", generation}, {"after", 1}}.dump();
    const auto request = Json::parse(dfy::mc::inbox_request("r", key, args));
    if (request.at("method") != "chat.read" || request.at("key") != key) throw std::runtime_error("missing pairing");
    Json reply{{"version", 1}, {"id", "r"}, {"inbox", {{"generation", generation}, {"owner", generation},
        {"oldest", 2}, {"latest", 2}, {"gap", false}, {"more", false}, {"messages", {{{"sequence", 2}, {"text", "查看"}}}}}}};
    if (Json::parse(dfy::mc::inbox_reply(reply.dump(), "r", args)) != reply.at("inbox")) throw std::runtime_error("inbox lost");
    reply["inbox"]["gap"] = true;
    rejects([&] { dfy::mc::inbox_reply(reply.dump(), "r", args); });
    reply["inbox"]["gap"] = false;
    reply["inbox"]["messages"][0]["sequence"] = 3;
    rejects([&] { dfy::mc::inbox_reply(reply.dump(), "r", args); });

    const auto target = Json{{"generation", generation}, {"order", 2}, {"place", "hillside"}}.dump();
    const auto stop = Json::parse(dfy::mc::inspect_request("inspect.stop", "a", key, target));
    if (stop.contains("place") || stop.at("order") != 2) throw std::runtime_error("wrong stop scope");
    Json action = Json::parse(target);
    action.update(Json{{"phase", "arrived"}, {"exited", true}, {"stone", nullptr}, {"position", {{"x", 10}, {"y", 2}, {"z", -1}}}});
    Json observed{{"version", 1}, {"id", "a"}, {"action", action}};
    if (!dfy::mc::inspect_reply(observed.dump(), "a", target).arrived) throw std::runtime_error("lost arrival");
    observed["action"]["exited"] = false;
    rejects([&] { dfy::mc::inspect_reply(observed.dump(), "a", target); });
    observed["action"]["exited"] = true;
    observed["action"]["order"] = 3;
    rejects([&] { dfy::mc::inspect_reply(observed.dump(), "a", target); });
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
        private_protocol();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
