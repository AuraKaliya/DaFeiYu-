#include "world_protocol.hpp"
#include <cmath>
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

// 从原工作读回已知与未知位置，拒绝替换目标、捏造到达和混用旧位置。
// 步骤：建立移动帧 → 核对数据缺失 → 注入冲突的目标、终态和时点。
void move_progress_protocol() {
    const Json source{{"generation","00112233-4455-6677-8899-aabbccddeeff"},{"order",2}};
    const auto controller = std::string(32,'a');
    const Json target{{"x",2},{"y",-60},{"z",0}};
    const auto arguments = Json{{"context",source},{"arguments",target}}.dump();
    Json action = source;
    action.update(Json{{"controller",controller},{"exited",false},{"skill","move"}});
    Json receipt{{"version",1},{"id","walk"},{"action",action}};
    auto& sample = receipt["action"]["result"]["progress"];
    sample = {{"phase","unknown"},{"path","timeout"},{"sampledAtMs",210},{"targetCell",target}};
    const auto read = [&] { return dfy::mc::partner_reply(receipt.dump(),"walk",controller,"move",arguments); };
    if (Json::parse(read().progress) != sample) throw std::runtime_error("unknown move sample lost its time or target");
    sample["position"] = target;
    rejects(read);
    sample.update(Json{{"phase","moving"},{"path","partial"},{"distance",std::sqrt(0.5)},{"lastMovedAtMs",200}});
    if (Json::parse(read().progress) != sample) throw std::runtime_error("measured movement was lost");
    sample["distance"] = 0;
    rejects(read);
    sample["distance"] = std::sqrt(0.5);
    sample["targetCell"]["x"] = 3;
    rejects(read);
    sample["targetCell"] = target;
    sample["lastMovedAtMs"] = 220;
    rejects(read);
    sample["lastMovedAtMs"] = 200;
    sample["phase"] = "arrived";
    rejects(read);
}

// 检查采掘轮询只接受原目标、已核实的破坏数和本次库存。
// 步骤：读取真实阶段 → 注入错格和倒置时点 → 对照退出时的库存。
void collect_progress_protocol() {
    const Json source{{"generation","00112233-4455-6677-8899-aabbccddeeff"},{"order",2}};
    const auto controller = std::string(32,'b');
    const Json target{{"x",2},{"y",-61},{"z",0}};
    Json args = target;
    args.update(Json{{"block","grass_block"},{"count",1},{"item","minecraft:dirt"},{"requiredStock",1}});
    const auto arguments = Json{{"context",source},{"arguments",args}}.dump();
    Json action = source;
    action.update(Json{{"controller",controller},{"exited",false},{"skill","collect"}});
    Json receipt{{"version",1},{"id","dig"},{"action",action}};
    auto& result = receipt["action"]["result"];
    result["broken"] = 1;
    auto& step = result["progress"];
    step = {{"phase","awaiting_pickup"},{"phaseSinceMs",110},{"sampledAtMs",125},{"lastBreakAtMs",108},{"broken",1},
        {"targetCell",target},{"block","grass_block"},{"stock",{{"item","dirt"},{"current",0},{"required",1}}}};
    const auto read = [&] { return dfy::mc::partner_reply(receipt.dump(),"dig",controller,"collect",arguments); };
    if (Json::parse(read().progress) != step) throw std::runtime_error("collection progress lost");
    step["targetCell"]["x"] = 3;
    rejects(read);
    step["targetCell"] = target;
    step["lastBreakAtMs"] = 130;
    rejects(read);
    step["lastBreakAtMs"] = 108;
    receipt["action"]["exited"] = true;
    result["status"] = "interrupted";
    result["stock"] = {{"after",0}};
    if (!read().exited) throw std::runtime_error("partial collect progress lost");
    step["stock"]["current"] = 1;
    rejects(read);
    step["stock"]["current"] = 0;
    Json stack{{"state","left_view"},{"entityId",9},{"item","dirt"},{"seenCount",1},{"targetCell",target},
        {"position",target},{"seenAtMs",107},{"leftAtMs",112},{"selfCount",0},{"otherCount",1},
        {"lastCollector",{{"entityId",10},{"self",false},{"count",1},{"sampledAtMs",110}}}};
    auto& pickups = result["pickups"] = {{"scope","one_new_stack_near_each_target"},{"attempted",1},{"omitted",0},
        {"recent",Json::array({stack})},{"selfCollected",Json::object()},{"otherCollected",{{"dirt",1}}}};
    if (!read().exited) throw std::runtime_error("other collector receipt lost");
    pickups["selfCollected"]["dirt"] = 1;
    rejects(read);
    pickups["selfCollected"] = Json::object();
    pickups["recent"][0]["targetCell"]["x"] = 3;
    rejects(read);
}

// 用固定读数验证私有收件窗口及查看行动的目标、序号和退出状态。
// 步骤：核对查询帧 → 注入收件断档或替换来源 → 验证到达和非法终态。
void private_protocol() {
    const std::string generation = "12345678-1234-1234-1234-123456789abc", key(64, 'a');
    const auto args = Json{{"generation", generation}, {"after", 1}}.dump();
    const auto request = Json::parse(dfy::mc::inbox_request("r", key, args));
    if (request.at("method") != "chat.read" || request.at("key") != key) throw std::runtime_error("missing pairing");
    const std::string controller(32, 'b');
    const auto listen = Json::parse(dfy::mc::inbox_request("r",key,args,controller));
    if (listen.at("method") != "partner.listen" || listen.at("controller") != controller)
        throw std::runtime_error("partner listen lost its control identity");
    const auto detach = Json{{"generation",generation},{"controller",controller}}.dump();
    const auto takeover = Json::parse(dfy::mc::control_request("r",key,Json{{"generation",generation}}.dump()));
    if (Json::parse(dfy::mc::control_request("r",key,detach)).at("method") != "control.detach" ||
        takeover.at("method") != "control.stop" || takeover.at("scope") != "role")
        throw std::runtime_error("detach and takeover share the wrong protocol method");
    Json detached{{"generation",generation},{"controller",controller},{"order",0},{"phase","released"}};
    Json stop_reply{{"version",1},{"id","r"},{"control",detached}};
    if (Json::parse(dfy::mc::control_reply(stop_reply.dump(),"r",detach)) != detached)
        throw std::runtime_error("detach did not retain the original controller");
    stop_reply["control"]["controller"] = std::string(32,'c');
    rejects([&] { dfy::mc::control_reply(stop_reply.dump(),"r",detach); });
    Json reply{{"version", 1}, {"id", "r"}, {"inbox", {{"generation", generation}, {"owner", generation},
        {"oldest", 2}, {"latest", 2}, {"gap", false}, {"more", false}, {"messages", {{{"sequence", 2}, {"text", "查看"}}}}}}};
    if (Json::parse(dfy::mc::inbox_reply(reply.dump(), "r", args)) != reply.at("inbox")) throw std::runtime_error("inbox lost");
    auto released = reply;
    released["inbox"]["execution"] = {{"mode","partner"},{"state","released"}};
    if (Json::parse(dfy::mc::inbox_reply(released.dump(), "r", args)) != released.at("inbox"))
        throw std::runtime_error("lost non-executable bridge status");
    released["inbox"]["execution"]["mode"] = "none";
    rejects([&] { dfy::mc::inbox_reply(released.dump(), "r", args); });
    released["inbox"]["execution"] = {{"mode","partner"},{"state","busy"}};
    if (Json::parse(dfy::mc::inbox_reply(released.dump(),"r",args)) != released.at("inbox"))
        throw std::runtime_error("busy bridge was not reported as readable");
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
        move_progress_protocol();
        collect_progress_protocol();
        const auto source = Json{{"generation","00112233-4455-6677-8899-aabbccddeeff"},{"order",2}};
        const auto config = dfy::mc::configured_bridge(Json{{"channel","test"},{"bridgeKey",std::string(64,'a')}}.dump());
        using dfy::mc::PlayerThinking;
        if (config.player_thinking != PlayerThinking::inherit ||
            dfy::mc::configured_bridge(R"({"channel":"test","playerThinking":"low"})").player_thinking != PlayerThinking::low ||
            dfy::mc::configured_bridge(R"({"channel":"test","playerThinking":"off"})").player_thinking != PlayerThinking::off) return 1;
        rejects([] { dfy::mc::configured_bridge(R"({"channel":"test","playerThinking":"high"})"); });
        const auto envelope = Json{{"context",source},{"arguments",{{"block","dirt"},{"count",2}}}}.dump();
        auto action = source;
        action["controller"] = config.controller;
        for (const auto method : {"partner.begin","partner.read","partner.stop"}) {
            const auto frame = Json::parse(dfy::mc::partner_request(method,"work",config,"collect",envelope));
            if (frame.at("context") != action || frame.at("arguments") != Json::parse(envelope).at("arguments")) return 1;
        }
        action.update(Json{{"skill","collect"},{"exited",true},{"result",{{"status","blocked"},{"broken",1}}}});
        Json receipt{{"version",1},{"id","work"},{"action",action}};
        auto detail = receipt;
        const auto detail_args = Json{{"context",source},{"arguments",{{"mode","detail"},{"observation","original"}}}}.dump();
        detail["action"]["skill"] = "observe";
        detail["action"]["result"] = {{"status","done"},{"survey",Json::object()},
            {"sourceObservation",{{"id","original"},{"dimension","overworld"},{"generation",source.at("generation")}}}};
        if (!dfy::mc::partner_reply(detail.dump(),"work",config.controller,"observe",detail_args).exited) return 1;
        detail["action"]["result"]["sourceObservation"]["id"] = "other";
        rejects([&] { dfy::mc::partner_reply(detail.dump(),"work",config.controller,"observe",detail_args); });
        detail["action"]["result"]["sourceObservation"]["id"] = "original";
        detail["action"]["result"]["sourceObservation"]["generation"] = "other-world";
        rejects([&] { dfy::mc::partner_reply(detail.dump(),"work",config.controller,"observe",detail_args); });
        const auto drop_args = Json{{"context",source},{"arguments",{{"mode","drops"},{"item","minecraft:dirt"}}}}.dump();
        const Json origin{{"x",0},{"y",0},{"z",0}};
        const Json dropped{{"entityId",13},{"position",origin},{"distance",0},{"item","dirt"},{"count",2}};
        auto& drops = detail["action"]["result"] = {{"status","done"},{"dropObservation",{{"id","work"},
            {"generation",source.at("generation")},{"dimension","overworld"},{"sampledAtMs",10},{"position",origin},
            {"radius",8},{"coverage","client_tracked_entities"},{"item","dirt"},{"omitted",0},{"unknownStack",0},
            {"unlocated",0},{"entities",Json::array({dropped})}}}};
        if (!dfy::mc::partner_reply(detail.dump(),"work",config.controller,"observe",drop_args).exited) return 1;
        auto& entries = drops["dropObservation"]["entities"];
        entries.push_back(dropped);
        rejects([&] { dfy::mc::partner_reply(detail.dump(),"work",config.controller,"observe",drop_args); });
        entries.erase(1);
        entries[0]["distance"] = 3;
        rejects([&] { dfy::mc::partner_reply(detail.dump(),"work",config.controller,"observe",drop_args); });
        drops.erase("dropObservation");
        rejects([&] { dfy::mc::partner_reply(detail.dump(),"work",config.controller,"observe",drop_args); });
        const auto partial = dfy::mc::partner_reply(receipt.dump(),"work",config.controller,"collect",envelope);
        const auto speech = Json{{"context",source},{"arguments",{{"text","我在这儿。"}}}}.dump();
        auto delivered = receipt;
        delivered["action"]["skill"] = "reply";
        delivered["action"]["result"] = {{"status","blocked"},{"sent",false},{"delivery","not_sent"}};
        if (dfy::mc::partner_reply(delivered.dump(),"work",config.controller,"reply",speech).delivery != dfy::mc::ReplyDelivery::not_sent)
            return 1;
        delivered["action"]["result"]["sent"] = true;
        rejects([&] { dfy::mc::partner_reply(delivered.dump(),"work",config.controller,"reply",speech); });
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
        const auto crafting = Json{{"context",source},{"arguments",{{"item","minecraft:oak_planks"},{"count",4},{"ingredient","oak_log"}}}}.dump();
        receipt["action"].update(Json{{"skill","craft"},{"result",{{"status","done"},{"item","oak_planks"},{"count",4},{"craftAttempted",true},
            {"craft",{{"ingredient","oak_log"},{"consumed",1},{"produced",4},{"remaining",0},{"batches",1},{"completedBatches",1}}},
            {"inventoryDelta",{{"oak_log",-1},{"oak_planks",4}}}}}});
        const auto crafted = dfy::mc::partner_reply(receipt.dump(),"work",config.controller,"craft",crafting);
        if (!crafted.changed || !crafted.craft_attempted) return 1;
        receipt["action"]["result"]["inventoryDelta"]["oak_planks"] = 0;
        rejects([&] { dfy::mc::partner_reply(receipt.dump(),"work",config.controller,"craft",crafting); });
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
