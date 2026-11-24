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
    const Json awaiting = step;
    step["phase"] = "checking_target";
    rejects(read);
    result["broken"] = step["broken"] = 0;
    step.erase("lastBreakAtMs");
    if (Json::parse(read().progress).at("targetCell") != target) throw std::runtime_error("checked target lost its identity");
    step = awaiting;
    result["broken"] = 1;
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
    step["pickup"] = {{"state","left_view"},{"entityId",9},{"item","dirt"},{"selfCount",0},{"otherCount",1},
        {"lastCollector",stack.at("lastCollector")}};
    if (Json::parse(read().progress).at("pickup") != step.at("pickup")) throw std::runtime_error("active pickup missing");
    step["pickup"]["selfCount"] = 1;
    rejects(read);
    step.erase("pickup");
    pickups["selfCollected"]["dirt"] = 1;
    rejects(read);
    pickups["selfCollected"] = Json::object();
    pickups["recent"][0]["targetCell"]["x"] = 3;
    rejects(read);
    result.erase("pickups");
    result["capacity"] = {{"item","dirt"},{"scope","main_and_hotbar"},{"sampledAtMs",123},{"status","sampled"},
        {"slots",36},{"stackSize",64},{"mergeSpace",0},{"emptySlots",0},{"unknownSlots",0},{"knownSpace",0}};
    result["status"] = "blocked";
    result["reasonCode"] = "pickup_inventory_full";
    if (!read().exited) throw std::runtime_error("full inventory lost its capacity sample");
    result["capacity"]["emptySlots"] = 1;
    rejects(read);
    result["capacity"]["knownSpace"] = 64;
    rejects(read);
    result.erase("capacity");
    rejects(read);
}

// 验证独立拾取的工作只按原实体、实际收取人与角色库存结算。
// 步骤：构造完整拾取 → 注入异堆和库存错配 → 核对受阻后的部分实效。
void ground_pickup_protocol() {
    const std::string generation = "00112233-4455-6677-8899-aabbccddeeff", entity = "00112233-4455-6677-8899-aabbccddee00";
    const auto controller = std::string(32,'c');
    const Json context{{"generation",generation},{"order",2}}, position{{"x",2},{"y",-60},{"z",1}};
    const Json args{{"observation","source"},{"entity",entity},{"item","minecraft:dirt"},{"count",3}};
    const auto envelope = Json{{"context",context},{"arguments",args}}.dump();
    const dfy::mc::BridgeConfig config{{},std::string(64,'a'),controller};
    auto invalid = args;
    const auto submit = [&] { return dfy::mc::partner_request("partner.start","ground",config,"pickup_drop",
        Json{{"context",context},{"arguments",invalid}}.dump()); };
    if (Json::parse(submit()).at("arguments") != args) throw std::runtime_error("pickup arguments lost");
    invalid["entity"] = std::string(36,'x');
    rejects(submit);
    invalid = args;
    invalid["observation"] = std::string(129,'a');
    rejects(submit);
    auto action = context;
    action.update(Json{{"controller",controller},{"skill","pickup_drop"},{"exited",true}});
    Json frame{{"version",1},{"id","ground"},{"action",action}};
    auto& result = frame["action"]["result"];
    result["status"] = "done";
    result["dropTarget"] = {{"observation","source"},{"entity",entity},{"generation",generation},{"dimension","overworld"},
        {"observedAtMs",100},{"sampledAtMs",105},{"radius",8},{"original",{{"entityId",8},{"item","dirt"},{"count",3}}},
        {"state","tracked"},{"entityId",8},{"position",position},{"distance",2},{"withinOriginalRadius",true},{"item","dirt"},{"count",3}};
    auto& pickup = result["pickup"] = {{"item","dirt"},{"requested",3},{"stockBefore",2},{"stockAfter",5},{"stockCurrent",true},
        {"stockSampledAtMs",125},{"verifiedCount",3},{"approachAttempted",true}};
    auto& observed = pickup["observed"] = {{"state","left_view"},{"entityId",8},{"uuid",entity},{"item","dirt"},{"seenCount",3},
        {"position",position},{"seenAtMs",106},{"leftAtMs",121},{"selfCount",3},{"otherCount",0},
        {"lastCollector",{{"entityId",1},{"self",true},{"count",3},{"sampledAtMs",120}}}};
    result["inventoryDelta"] = {{"dirt",3}};
    auto& progress = result["progress"] = {{"phase","received"},{"entity",entity},{"item","dirt"},{"requested",3},
        {"selfCount",3},{"otherCount",0},{"stock",5},{"sampledAtMs",125}};
    const auto read = [&] { return dfy::mc::partner_reply(frame.dump(),"ground",controller,"pickup_drop",envelope); };
    const auto success = read();
    if (!success.arrived || !success.changed || !success.inventory_attempted || Json::parse(success.progress) != progress)
        throw std::runtime_error("ground pickup lost packet, inventory or work progress");
    observed["uuid"] = generation;
    rejects(read);
    observed["uuid"] = entity;
    result["inventoryDelta"]["dirt"] = 2;
    rejects(read);
    result["inventoryDelta"]["dirt"] = 3;
    pickup["stockCurrent"] = false;
    rejects(read);
    pickup["stockCurrent"] = true;
    observed["selfCount"] = progress["selfCount"] = pickup["verifiedCount"] = 0;
    observed["otherCount"] = progress["otherCount"] = 3;
    observed["lastCollector"]["self"] = false;
    rejects(read);
    result["status"] = "blocked";
    if (read().changed) throw std::runtime_error("another collector credited the companion");
    observed["selfCount"] = progress["selfCount"] = pickup["verifiedCount"] = 1;
    observed["otherCount"] = progress["otherCount"] = observed["lastCollector"]["count"] = 2;
    if (!read().changed || read().arrived) throw std::runtime_error("partial pickup lost its effect");
    result["reasonCode"] = "pickup_other_stack_nearby";
    rejects(read);
    result["nearbyDrop"] = {{"entityId",9},{"uuid",generation},{"stack",nullptr},{"position",position},{"sampledAtMs",121}};
    if (!read().changed) throw std::runtime_error("new neighboring stack discarded an earlier pickup");
    result["nearbyDrop"]["uuid"] = entity;
    rejects(read);
}

// 核对交物观察按接收者、距离与槽位总数结算，拒绝跨人或重复选槽。
// 步骤：构造跨堆备料 → 替换玩家和脚位 → 伪造选择或缺料量并验证拒绝。
void handoff_protocol() {
    const std::string world = "00112233-4455-6677-8899-aabbccddeeff", owner = "00112233-4455-6677-8899-aabbccddee00";
    const auto controller = std::string(32,'d');
    const Json context{{"generation",world},{"order",2}};
    const auto args = Json{{"context",context},{"arguments",{{"mode","handoff"},{"recipient",owner},{"item","minecraft:dirt"},{"count",66}}}}.dump();
    auto action = context;
    action.update(Json{{"controller",controller},{"skill","observe"},{"exited",true}});
    Json frame{{"version",1},{"id","offer"},{"action",action}};
    auto& result = frame["action"]["result"];
    result["status"] = "done";
    auto& sample = result["handoff"] = {{"id","offer"},{"generation",world},{"dimension","overworld"},{"sampledAtMs",20},
        {"recipient",owner},{"item","dirt"},{"count",66},{"window","inventory"},
        {"actor",{{"uuid",world},{"position",{{"x",0},{"y",-60},{"z",0}}}}},
        {"target",{{"state","visible"},{"entityId",8},{"position",{{"x",3},{"y",-60},{"z",0}}},{"distance",3}}}};
    auto& stock = sample["stock"] = {{"status","sampled"},{"scope","main_and_hotbar"},{"start",9},{"end",45},{"stackSize",64},
        {"available",70},{"missing",0},{"excluded",0},{"unknownSlots",0},{"selected",Json::array({
            {{"slot",9},{"held",64},{"take",64}},{{"slot",10},{"held",6},{"take",2}}})}};
    const auto read = [&] { return dfy::mc::partner_reply(frame.dump(),"offer",controller,"observe",args); };
    if (!read().arrived || read().changed) throw std::runtime_error("read-only handoff inventory lost");
    sample["recipient"] = world;
    rejects(read);
    sample["recipient"] = owner;
    sample["target"]["distance"] = 2;
    rejects(read);
    sample["target"] = {{"state","not_tracked"}};
    if (!read().arrived) throw std::runtime_error("unknown recipient position lost");
    stock["selected"][1]["slot"] = 9;
    rejects(read);
    stock["selected"][1]["slot"] = 10;
    stock["missing"] = 1;
    rejects(read);
    stock["missing"] = 0;
    sample["target"] = {{"state","visible"},{"entityId",8},{"position",{{"x",2},{"y",-60},{"z",0}}},{"distance",2}};
    frame["action"]["skill"] = "give_item";
    frame["id"] = "give";
    result["status"] = "blocked";
    result["reasonCode"] = "handoff_receipt_unconfirmed";
    auto& transfer = result["transfer"] = {{"observation","offer"},{"recipient",owner},{"item","dirt"},{"requested",66},
        {"attemptedClicks",3},{"returnedClicks",3},{"submitted",66},{"sourceClaimed",true},
        {"stockBefore",70},{"stockAfter",4},{"stockCurrent",true},{"sampledAtMs",50}};
    result["inventoryDelta"] = {{"dirt",-66}};
    auto command = Json::parse(args);
    command["arguments"].erase("mode");
    command["arguments"]["observation"] = "offer";
    const auto give = [&] { return dfy::mc::partner_reply(frame.dump(),"give",controller,"give_item",command.dump()); };
    if (!give().inventory_attempted || give().changed || give().arrived) throw std::runtime_error("uncertain transfer reported receipt");
    result["status"] = "done";
    rejects(give);
    result["status"] = "blocked";
    transfer["submitted"] = 67;
    rejects(give);
    transfer["submitted"] = 66;
    result["inventoryDelta"]["dirt"] = -65;
    rejects(give);
    result["inventoryDelta"]["dirt"] = -66;
    auto& proof = result["receipt"] = {{"source","unavailable"},{"coverage","unavailable"},{"thrown",0},{"recipientPicked",0},
        {"otherPicked",0},{"throwUnknown",66},{"pickupUnknown",0}};
    if (give().changed) throw std::runtime_error("unsupported observer reported a handoff");
    proof = {{"source","server_events"},{"coverage","read"},{"stream",world},{"afterSeq",12},{"throughSeq",25},
        {"thrown",66},{"recipientPicked",66},{"otherPicked",0},{"throwUnknown",0},{"pickupUnknown",0},{"omittedStacks",0},
        {"stacks",Json::array({{{"uuid",owner},{"count",64},{"dropSeq",13},{"spawnSeq",14}},
            {{"uuid",world},{"count",2},{"dropSeq",15},{"spawnSeq",16}}})}};
    result["status"] = "done";
    result.erase("reasonCode");
    if (!give().changed || !give().arrived) throw std::runtime_error("confirmed recipient pickup lost");
    proof["recipientPicked"] = 65;
    rejects(give);
    proof["recipientPicked"] = 66;
    proof["stacks"][1]["uuid"] = owner;
    rejects(give);
    proof["stacks"][1]["uuid"] = world;
    const Json late = proof;
    frame["action"]["skill"] = "observe";
    frame["id"] = "lookup";
    command["arguments"] = {{"mode","handoff_receipt"},{"work","give"}};
    result = {{"status","done"},{"handoffReceipt",{{"id","lookup"},{"work","give"},{"order",2},{"observation","offer"},
        {"generation",world},{"dimension","overworld"},{"recipient",owner},{"item","dirt"},{"requested",66},{"submitted",66},
        {"originalStatus","blocked"},{"sampledAtMs",150},{"sourceThrough",16},{"receipt",late}}}};
    const auto lookup = [&] { return dfy::mc::partner_reply(frame.dump(),"lookup",controller,"observe",command.dump()); };
    if (!lookup().arrived || lookup().changed) throw std::runtime_error("read-only late handoff lost");
    result["handoffReceipt"]["sourceThrough"] = 14;
    rejects(lookup);
    result["handoffReceipt"]["sourceThrough"] = 16;
    result["handoffReceipt"]["work"] = "another-give";
    rejects(lookup);
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
        ground_pickup_protocol();
        handoff_protocol();
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
        const Json target_args{{"context",source},{"arguments",{{"mode","drop"},{"observation","before"},{"entity",source.at("generation")}}}};
        auto& lost = drops["dropTarget"] = {{"observation","before"},{"entity",source.at("generation")},{"generation",source.at("generation")},
            {"dimension","overworld"},{"observedAtMs",10},{"sampledAtMs",11},{"radius",8},{"state","not_tracked"},
            {"original",{{"entityId",13},{"item","dirt"},{"count",2}}}};
        const auto recheck = [&] { return dfy::mc::partner_reply(detail.dump(),"work",config.controller,"observe",target_args.dump()); };
        if (!recheck().exited) return 1;
        lost["entityId"] = 13;
        rejects(recheck);
        lost.erase("entityId");
        lost["entity"] = "11112233-4455-6677-8899-aabbccddeeff";
        rejects(recheck);
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
