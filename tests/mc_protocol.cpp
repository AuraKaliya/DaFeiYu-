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
    auto released = reply;
    released["inbox"]["execution"] = {{"mode","partner"},{"state","released"}};
    if (Json::parse(dfy::mc::inbox_reply(released.dump(), "r", args)) != released.at("inbox"))
        throw std::runtime_error("lost non-executable bridge status");
    released["inbox"]["execution"]["mode"] = "none";
    rejects([&] { dfy::mc::inbox_reply(released.dump(), "r", args); });
    auto presence = reply;
    const Json entered{{"sequence",2},{"type","player.entered"}};
    presence["inbox"]["presence"] = entered;
    presence["inbox"]["messages"][0] = entered;
    if (Json::parse(dfy::mc::inbox_reply(presence.dump(), "r", args)) != presence.at("inbox")) throw std::runtime_error("lost presence");
    presence["inbox"]["messages"][0]["text"] = "invented";
    rejects([&] { dfy::mc::inbox_reply(presence.dump(), "r", args); });
    presence["inbox"]["messages"][0] = entered;
    presence["inbox"]["presence"]["type"] = "player.left";
    rejects([&] { dfy::mc::inbox_reply(presence.dump(), "r", args); });
    presence["inbox"]["presence"]["sequence"] = 3;
    rejects([&] { dfy::mc::inbox_reply(presence.dump(), "r", args); });
    reply["inbox"]["gap"] = true;
    rejects([&] { dfy::mc::inbox_reply(reply.dump(), "r", args); });
    reply["inbox"]["gap"] = false;
    reply["inbox"]["messages"][0]["sequence"] = 3;
    rejects([&] { dfy::mc::inbox_reply(reply.dump(), "r", args); });

    const auto target = Json{{"generation", generation}, {"order", 2}, {"place", "hillside"}}.dump();
    const auto stop = Json::parse(dfy::mc::inspect_request("inspect.stop", "a", key, target));
    if (stop.contains("place") || stop.at("order") != 2) throw std::runtime_error("wrong stop scope");
    Json action = Json::parse(target);
    action.update(Json{{"phase", "arrived"}, {"exited", true}, {"moved", true}, {"stone", nullptr}, {"position", {{"x", 10}, {"y", 2}, {"z", -1}}}});
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
        auto exploration = value;
        auto& survey = exploration["state"]["survey"];
        survey = {{"order", 2}, {"window", 1}, {"area", "0,-1,2"}, {"sample", 7}, {"returns", {3, 5, 7}}, {"seen", 4}, {"gained", 3}};
        if (Json::parse(dfy::mc::world_reply(exploration.dump(), id, true)).at("survey") != survey) return 1;
        rejects([&] { dfy::mc::world_reply(exploration.dump(), id); });
        survey["returns"] = {3, 3, 7};
        rejects([&] { dfy::mc::world_reply(exploration.dump(), id, true); });
        rejects([&] { dfy::mc::survey_request(id, std::string(64, 'a'), R"({"order":2,"inquiry":"wrong"})"); });
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
        const auto source = Json{{"generation","00112233-4455-6677-8899-aabbccddeeff"},{"order",2}};
        const auto config = dfy::mc::configured_bridge(Json{{"channel","test"},{"bridgeKey",std::string(64,'a')}}.dump());
        const auto envelope = Json{{"context",source},{"arguments",{{"block","dirt"},{"count",2}}}}.dump();
        auto action = source;
        action["controller"] = config.controller;
        for (const auto method : {"partner.begin","partner.read","partner.stop"}) {
            const auto frame = Json::parse(dfy::mc::partner_request(method,"work",config,"collect",envelope));
            if (frame.at("context") != action || frame.at("arguments") != Json::parse(envelope).at("arguments")) return 1;
        }
        action.update(Json{{"skill","collect"},{"exited",true},{"result",{{"status","blocked"},{"broken",1}}}});
        Json receipt{{"version",1},{"id","work"},{"action",action}};
        const auto partial = dfy::mc::partner_reply(receipt.dump(),"work",config.controller,"collect",envelope);
        if (!partial.exited || partial.arrived || Json::parse(partial.payload).at("broken") != 1) return 1;
        rejects([&] { dfy::mc::partner_reply(receipt.dump(),"work",std::string(32,'b'),"collect",envelope); });
        receipt["action"]["order"] = 1;
        rejects([&] { dfy::mc::partner_reply(receipt.dump(),"work",config.controller,"collect",envelope); });
        const auto placement = Json{{"context",source},{"arguments",{{"block","minecraft:oak_door"},{"x",1},{"y",-60},{"z",3}}}}.dump();
        action = source;
        action["controller"] = config.controller;
        action.update(Json{{"skill","place"},{"exited",true},{"result",{{"status","interrupted"},{"placementAttempted",true},
            {"placed",true},{"blocks",Json::array({{{"block","oak_door"},{"x",1},{"y",-60},{"z",3},{"half","lower"}},
                {{"block","oak_door"},{"x",1},{"y",-59},{"z",3},{"half","upper"}}})}}}});
        receipt["action"] = action;
        const auto placed = dfy::mc::partner_reply(receipt.dump(),"work",config.controller,"place",placement);
        if (!placed.exited || placed.arrived || !placed.placement_verified || !placed.placement_attempted) return 1;
        receipt["action"]["result"]["blocks"][1]["z"] = 4;
        rejects([&] { dfy::mc::partner_reply(receipt.dump(),"work",config.controller,"place",placement); });
        const Json first{{"x",1},{"y",-60},{"z",3}}, second{{"x",2},{"y",-60},{"z",3}};
        const auto fill = Json{{"context",source},{"arguments",{{"block","dirt"},{"cells",Json::array({first,second})},
            {"sourceMessage",1},{"taskRevision",2}}}}.dump();
        auto blocked = first;
        blocked.update(Json{{"before","air"},{"block","air"},{"status","blocked"},{"reasonCode","no_support"}});
        auto complete = second;
        complete.update(Json{{"before","air"},{"block","dirt"},{"status","placed"}});
        auto change = second;
        change.update(Json{{"before","air"},{"after","dirt"}});
        action = source;
        action["controller"] = config.controller;
        action.update(Json{{"skill","fill_target"},{"exited",true},{"result",{{"status","blocked"},{"placementAttempted",true},
            {"sourceMessage",1},{"taskRevision",2},{"cells",Json::array({blocked,complete})},{"changes",Json::array({change})}}}});
        receipt["action"] = action;
        if (!dfy::mc::partner_reply(receipt.dump(),"work",config.controller,"fill_target",fill).changed) return 1;
        receipt["action"]["result"]["cells"][0]["x"] = 2;
        rejects([&] { dfy::mc::partner_reply(receipt.dump(),"work",config.controller,"fill_target",fill); });
        receipt["action"] = action;
        receipt["action"]["result"]["changes"][0]["x"] = 3;
        rejects([&] { dfy::mc::partner_reply(receipt.dump(),"work",config.controller,"fill_target",fill); });
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
