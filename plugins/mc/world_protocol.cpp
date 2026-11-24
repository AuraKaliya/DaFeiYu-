#include "world_protocol.hpp"
#include "partner_effect.hpp"
#include "pipe_exchange.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;
// 在解释消息前限制字节、嵌套层数和同层重复键。
// 步骤：检查输入大小 → 按嵌套对象核对键 → 生成完整 JSON 值。
Json parse(std::string_view text, int depth_limit = 6) {
    if (text.empty() || text.size() > 16384) throw std::invalid_argument("MC protocol: size");
    std::vector<std::set<std::string>> objects;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > depth_limit) throw std::invalid_argument("MC protocol: depth");
        if (event == Json::parse_event_t::object_start) objects.emplace_back();
        else if (event == Json::parse_event_t::key && !objects.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("MC protocol: duplicate field");
        else if (event == Json::parse_event_t::object_end) objects.pop_back();
        return true;
    });
}
// 验证维度是可显示的 Minecraft 资源名称。
bool dimension(const Json& value) {
    if (!value.is_string()) return false;
    const auto& name = value.get_ref<const std::string&>();
    return !name.empty() && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || std::string_view(":/._-").find(c) != std::string_view::npos;
    });
}

// 验证主背包与快捷栏的容量覆盖，只接受与已读槽位一致的可用量。
void verify_inventory_capacity(const Json& capacity, std::string_view item) {
    if (item.empty() || capacity.at("item").get_ref<const std::string&>() != item || capacity.at("scope") != "main_and_hotbar" ||
        !capacity.at("sampledAtMs").is_number_unsigned()) throw std::invalid_argument("MC: invalid inventory capacity source");
    const auto& status = capacity.at("status");
    if (status == "unavailable" && capacity.size() == 4) return;
    for (const auto field : {"slots","stackSize","emptySlots","unknownSlots","mergeSpace","knownSpace"})
        if (!capacity.at(field).is_number_unsigned() || capacity.at(field) > 16384)
            throw std::invalid_argument("MC: invalid inventory capacity measure");
    const auto slots = capacity.at("slots").get<int>(), size = capacity.at("stackSize").get<int>();
    const auto empty = capacity.at("emptySlots").get<int>(), unknown = capacity.at("unknownSlots").get<int>();
    const auto merge = capacity.at("mergeSpace").get<int>();
    if (slots < 1 || slots > 128 || size < 1 || size > 128 || empty+unknown > slots ||
        merge > (slots-empty-unknown)*(size-1) || capacity.at("knownSpace") != empty*size+merge ||
        status != (unknown ? "partial" : "sampled")) throw std::invalid_argument("MC: inconsistent inventory capacity");
}

// 对照原查询检查掉落来源、覆盖和实体列表，缺失物品栈保持未知。
// 步骤：绑定原工作和查询范围 → 校验角色与实体位置 → 核对物品过滤和距离。
void verify_drop_observation(const Json& sample, std::string_view id, const Json& generation, const Json& args) {
    const auto radius = args.value("radius",8);
    const auto requested_item = args.value("item",std::string{});
    const auto filter = item_name(requested_item);
    if (sample.contains("capacity")) verify_inventory_capacity(sample.at("capacity"),filter);
    const auto& entities = sample.at("entities");
    if (args.value("mode","") != "drops" || sample.at("id").get_ref<const std::string&>() != id ||
        sample.at("generation") != generation || !dimension(sample.at("dimension")) ||
        !sample.at("sampledAtMs").is_number_unsigned() || sample.at("coverage") != "client_tracked_entities" ||
        sample.at("radius") != radius || radius < 1 || radius > 16 || sample.value("item",std::string{}) != filter ||
        !sample.at("unknownStack").is_number_unsigned() || !sample.at("unlocated").is_number_unsigned() ||
        !sample.at("omitted").is_number_unsigned() || !entities.is_array() || entities.size() > 12 ||
        (sample.at("omitted") != 0 && entities.size() != 12))
        throw std::invalid_argument("MC: invalid drop observation scope");

    const auto& center = sample.at("position");
    for (const auto axis : {"x","y","z"}) if (!center.at(axis).is_number())
        throw std::invalid_argument("MC: missing drop observation center");
    std::set<std::uint32_t> ids;
    double previous = 0;
    for (const auto& entity : entities) {
        const auto& item = entity.at("item");
        if (!entity.at("entityId").is_number_unsigned() || entity.at("entityId") > 0x7fffffff ||
            !ids.insert(entity.at("entityId").get<std::uint32_t>()).second || (entity.contains("uuid") && !game_uuid(entity.at("uuid"))) ||
            (item.is_null() ? !entity.at("count").is_null() :
                !dimension(item) || !entity.at("count").is_number_unsigned() || entity.at("count") == 0) ||
            (!filter.empty() && (item.is_null() || item.get_ref<const std::string&>() != filter)))
            throw std::invalid_argument("MC: invalid dropped stack");
        const auto& point = entity.at("position");
        const double measured = std::hypot(point.at("x").get<double>()-center.at("x").get<double>(),
            point.at("y").get<double>()-center.at("y").get<double>(),point.at("z").get<double>()-center.at("z").get<double>());
        const auto distance = entity.at("distance").get<double>();
        if (distance < previous || distance > radius || std::abs(distance-measured) > 1e-8)
            throw std::invalid_argument("MC: inconsistent drop observation distance");
        previous = distance;
    }
}
// 核对单堆复查的原观察、稳定身份和当前读数，失去视野时不推断拾取人。
// 步骤：检查原观察及世界 → 验证初见栈 → 根据本次覆盖核对已读属性。
void verify_drop_target(const Json& sample, const Json& args, const Json& generation) {
    const auto& original = sample.at("original");
    const auto& state = sample.at("state");
    if (!game_uuid(sample.at("entity")) || sample.at("entity") != args.at("entity") || sample.at("observation") != args.at("observation") ||
        sample.at("generation") != generation || !dimension(sample.at("dimension")) || !sample.at("observedAtMs").is_number_unsigned() ||
        !sample.at("sampledAtMs").is_number_unsigned() || sample.at("observedAtMs") > sample.at("sampledAtMs") ||
        !sample.at("radius").is_number_unsigned() || sample.at("radius") < 1 || sample.at("radius") > 16 ||
        !original.at("entityId").is_number_unsigned() || (original.at("item").is_null() ? !original.at("count").is_null() :
        !dimension(original.at("item")) || !original.at("count").is_number_unsigned() || original.at("count") == 0))
        throw std::invalid_argument("MC: invalid drop target source");
    const bool located = state == "tracked" || state == "stack_unknown";
    if ((!located && state != "unlocated" && state != "not_tracked") ||
        (sample.contains("entityId") != (state != "not_tracked")) || (sample.contains("entityId") && !sample.at("entityId").is_number_unsigned()) ||
        sample.contains("position") != located || sample.contains("distance") != located || sample.contains("withinOriginalRadius") != located ||
        sample.contains("item") != (state == "tracked") || sample.contains("count") != (state == "tracked"))
        throw std::invalid_argument("MC: inconsistent drop target coverage");
    if (located) {
        for (const auto axis : {"x","y","z"}) if (!sample.at("position").at(axis).is_number())
            throw std::invalid_argument("MC: invalid drop target position");
        if (!sample.at("distance").is_number() || sample.at("distance") < 0 || !sample.at("withinOriginalRadius").is_boolean())
            throw std::invalid_argument("MC: invalid drop target distance");
    }
    if (state == "tracked" && (!dimension(sample.at("item")) || !sample.at("count").is_number_unsigned() || sample.at("count") == 0))
        throw std::invalid_argument("MC: invalid current dropped stack");
}

// 将独立拾取的选定实体、收取数量和库存增减同原命令核对。
// 步骤：绑定原观察与目标 → 校验拾取人和原堆 → 对照进展、库存差量与成功条件。
bool verify_ground_pickup(const Json& result, const Json& args, const Json& generation, bool exited) {
    if (!result.contains("pickup")) {
        if (result.contains("progress") || (exited && result.at("status") == "done"))
            throw std::invalid_argument("MC: missing pickup evidence");
        return false;
    }
    const auto& target = result.at("dropTarget");
    verify_drop_target(target,args,generation);
    const auto& pickup = result.at("pickup");
    const auto& entry = pickup.at("observed");
    const auto& name = pickup.at("item").get_ref<const std::string&>();
    const auto requested = args.at("count").get<std::int64_t>();
    if (name.empty() || name != item_name(args.at("item").get_ref<const std::string&>()) || requested < 1 || requested > 0x7fffffff ||
        pickup.at("requested") != requested || !pickup.at("approachAttempted").is_boolean() || !pickup.at("stockCurrent").is_boolean() ||
        !pickup.at("stockSampledAtMs").is_number_unsigned()) throw std::invalid_argument("MC: invalid pickup request binding");
    if (result.contains("capacity")) verify_inventory_capacity(result.at("capacity"),name);
    for (const auto key : {"stockBefore","stockAfter","verifiedCount"})
        if (!pickup.at(key).is_number_unsigned() || pickup.at(key) > 0x7fffffff) throw std::invalid_argument("MC: invalid pickup stock");
    const bool seen = entry.at("state") != "unseen";
    if ((seen || pickup.at("approachAttempted") == true) && (target.at("state") != "tracked" || target.at("withinOriginalRadius") != true ||
        target.at("original").at("item") != name || target.at("original").at("count") != requested ||
        target.at("item") != name || target.at("count") != requested)) throw std::invalid_argument("MC: pickup target changed");
    int own = 0, other = 0;
    if (seen) {
        if ((entry.at("state") != "visible" && entry.at("state") != "left_view") || entry.at("uuid") != target.at("entity") ||
            entry.at("entityId") != target.at("entityId") || entry.at("item") != name || entry.at("seenCount") != requested ||
            !entry.at("seenAtMs").is_number_unsigned() || entry.at("seenAtMs") < target.at("sampledAtMs") ||
            entry.contains("leftAtMs") != (entry.at("state") == "left_view") || (entry.contains("leftAtMs") &&
                (!entry.at("leftAtMs").is_number_unsigned() || entry.at("leftAtMs") < entry.at("seenAtMs"))) ||
            !entry.at("selfCount").is_number_unsigned() || entry.at("selfCount") > 0x7fffffff ||
            !entry.at("otherCount").is_number_unsigned() || entry.at("otherCount") > 0x7fffffff)
            throw std::invalid_argument("MC: pickup packet refers to another stack");
        own = entry.at("selfCount").get<int>(); other = entry.at("otherCount").get<int>();
        if (entry.contains("lastCollector") != (own || other)) throw std::invalid_argument("MC: missing pickup collector");
        if (own || other) {
            const auto& last = entry.at("lastCollector");
            if (!last.at("entityId").is_number_unsigned() || (last.contains("uuid") && !game_uuid(last.at("uuid"))) ||
                !last.at("count").is_number_unsigned() || last.at("count") == 0 || last.at("count") > (last.at("self").get<bool>() ? own : other) ||
                !last.at("sampledAtMs").is_number_unsigned() || last.at("sampledAtMs") < entry.at("seenAtMs") ||
                (entry.contains("leftAtMs") && last.at("sampledAtMs") > entry.at("leftAtMs")))
                throw std::invalid_argument("MC: invalid pickup collector");
        }
    } else if (entry.size() != 1) throw std::invalid_argument("MC: unseen stack has pickup events");

    const auto delta = pickup.at("stockAfter").get<int>()-pickup.at("stockBefore").get<int>();
    const int verified = pickup.at("verifiedCount").get<int>(), both = std::min(own,std::max(0,delta));
    const auto& step = result.at("progress");
    const auto& phase = step.at("phase");
    if (!result.at("inventoryDelta").is_object() || result.at("inventoryDelta").value(name,0) != delta || verified > both ||
        (pickup.at("stockCurrent") == true && verified != both) || step.at("entity") != target.at("entity") || step.at("item") != name ||
        step.at("requested") != requested || step.at("selfCount") != own || step.at("otherCount") != other ||
        step.at("stock") != pickup.at("stockAfter") || !step.at("sampledAtMs").is_number_unsigned() ||
        step.at("sampledAtMs") < pickup.at("stockSampledAtMs") || (phase != "checking_target" && phase != "approaching" &&
        phase != "awaiting_pickup" && phase != "waiting_inventory" && phase != "received"))
        throw std::invalid_argument("MC: inconsistent pickup progress or inventory");
    if (exited && result.at("status") == "done" && (phase != "received" || own != requested || verified != requested ||
        other != 0 || pickup.at("stockCurrent") != true)) throw std::invalid_argument("MC: pickup completion unconfirmed");
    return own > 0;
}

// 询问修订使用非零十进制文本，避免 JavaScript 数值精度损失。
bool inquiry_id(const Json& value) {
    if (!value.is_string()) return false;
    const auto& id = value.get_ref<const std::string&>();
    return !id.empty() && id.size() <= 20 && id.front() != '0' && id.find_first_not_of("0123456789") == id.npos;
}
// 核对控制实例的本地协议身份。
bool controller_id(std::string_view value) {
    return value.size() == 32 && value.find_first_not_of("0123456789abcdef") == value.npos;
}

// 核对释放回执的原世界和控制范围。
bool control_receipt(const Json& value, const Json& generation, const Json& controller = nullptr) {
    return value.is_object() && value.size() == (controller.is_null() ? 3u : 4u) && value.at("generation") == generation &&
        (controller.is_null() || value.at("controller") == controller) &&
        value.at("order").is_number_unsigned() && value.at("order") <= 9007199254740991ULL &&
        (value.at("phase") == "requested" || value.at("phase") == "released" || value.at("phase") == "unconfirmed");
}

// 将已证明的放置结果逐格对应到原命令；门必须产生两份位置和半门属性。
void verify_placement(const Json& result, const Json& args) {
    if (args.contains("sourceMessage") && (result.at("sourceMessage") != args.at("sourceMessage") ||
        result.at("taskRevision") != args.at("taskRevision")))
        throw std::invalid_argument("MC: placement evidence differs from original message");
    auto name = args.at("block").get<std::string>();
    if (name.starts_with("minecraft:")) name.erase(0,10);
    const bool door = name.ends_with("_door");
    const auto& cells = result.at("blocks");
    if (!cells.is_array() || cells.size() != (door ? 2u : 1u)) throw std::invalid_argument("MC: invalid placement evidence");
    for (std::size_t index = 0; index < cells.size(); ++index) {
        const auto& cell = cells.at(index);
        if (cell.at("block") != name || cell.at("x") != args.at("x") || cell.at("z") != args.at("z") ||
            cell.at("y") != args.at("y").get<std::int64_t>()+static_cast<std::int64_t>(index) ||
            (door && cell.at("half") != (index ? "upper" : "lower")))
            throw std::invalid_argument("MC: placement evidence differs from target");
    }
}

// 检查合成退出回执是否对应原目标，并保留部分完成的物品变化。
// 步骤：匹配原物品和产量 → 核对耗料及产出差量 → 验证完成条件。
bool verified_craft(const Json& result, const Json& args, bool attempted) {
    const auto item = item_name(args.at("item").get_ref<const std::string&>());
    const auto& craft = result.at("craft");
    const auto ingredient = craft.value("ingredient","");
    const auto& delta = result.at("inventoryDelta");
    const auto requested = args.at("count").get<int>();
    if (result.at("item").get_ref<const std::string&>() != item || result.at("count") != requested || !delta.is_object() ||
        delta.size() != delta.count(item)+delta.count(ingredient) || (!attempted && !delta.empty()) ||
        (!ingredient.empty() && args.contains("ingredient") && ingredient != item_name(args.at("ingredient").get_ref<const std::string&>())))
        throw std::invalid_argument("MC: crafting evidence differs from original request");

    const auto consumed = std::max<std::int64_t>(0,-static_cast<std::int64_t>(delta.value(ingredient,0)));
    const auto produced = std::max(0,delta.value(item,0));
    if (craft.at("consumed") != consumed || craft.at("produced") != produced ||
        craft.at("remaining") != std::max(0,requested-produced) ||
        (result.at("status") == "done" && (!attempted || ingredient.empty() || consumed == 0 || produced < requested ||
            craft.at("completedBatches") != craft.at("batches"))))
        throw std::invalid_argument("MC: invalid crafting inventory evidence");
    return !delta.empty();
}

// 校验已读回的实际方格变化，可与未完成的工作一同返回。
bool verified_changes(const Json& changes) {
    if (!changes.is_array() || changes.size() > 64) throw std::invalid_argument("MC: invalid cell changes");
    for (const auto& cell : changes) {
        if (cell.size() != 5 || !dimension(cell.at("before")) || !dimension(cell.at("after")) ||
            cell.at("before") == cell.at("after"))
            throw std::invalid_argument("MC: invalid changed block");
        for (const auto axis : {"x","y","z"})
            if (!cell.at(axis).is_number_integer() || std::abs(cell.at(axis).get<double>()) > 30000000)
                throw std::invalid_argument("MC: invalid changed position");
    }
    return !changes.empty();
}

// 核对批量放置的原目标和逐格实际结果，部分成功只能返回原目标的前缀。
// 步骤：核对原话及修订 → 对照每个实际读回 → 检查完成是否覆盖全部目标。
void verify_fill(const Json& result, const Json& args, bool done) {
    const auto& cells = result.at("cells");
    const auto& target = args.at("cells");
    auto block = args.at("block").get<std::string>();
    if (block.starts_with("minecraft:")) block.erase(0,10);
    if (!cells.is_array() || cells.size() > target.size() || (done && cells.size() != target.size()) ||
        result.at("sourceMessage") != args.at("sourceMessage") || result.at("taskRevision") != args.at("taskRevision"))
        throw std::invalid_argument("MC: invalid fill evidence");
    const auto& changes = result.at("changes");
    const auto coordinates = [](const Json& a, const Json& b) {
        return a.at("x") == b.at("x") && a.at("y") == b.at("y") && a.at("z") == b.at("z");
    };
    if (!changes.is_array() || changes.size() > cells.size()) throw std::invalid_argument("MC: invalid fill changes");
    for (const auto& cell : cells) {
        const auto at_cell = [&](const auto& other) { return coordinates(cell,other); };
        if (std::ranges::none_of(target,at_cell) || std::ranges::count_if(cells,at_cell) != 1)
            throw std::invalid_argument("MC: fill evidence differs from target");
        const auto& status = cell.at("status");
        if (!done && status == "blocked") continue;
        if ((status != "placed" && status != "present") || cell.at("block") != block)
            throw std::invalid_argument("MC: unverified fill cell");
        if ((status == "placed" && std::ranges::count_if(changes,at_cell) != 1) ||
            (status == "present" && (cell.at("before") != block || std::ranges::any_of(changes,at_cell))))
            throw std::invalid_argument("MC: inconsistent fill readback");
    }
    for (const auto& change : changes) {
        const auto cell = std::ranges::find_if(cells,[&](const auto& value) { return coordinates(change,value); });
        if (cell == cells.end() || cell->at("before") != change.at("before") || cell->at("block") != change.at("after") ||
            std::ranges::count_if(changes,[&](const auto& value) { return coordinates(change,value); }) != 1)
            throw std::invalid_argument("MC: fill changed a cell outside the receipt");
    }
}

// 按原采掘范围核对近期掉落实体和拾取数量，离开视野本身不产生拾取实效。
// 步骤：核对观察覆盖 → 检查每个目标与实体来源 → 对照拾取人及累计数量。
void verify_collect_pickups(const Json& evidence, const Json& args) {
    const auto& recent = evidence.at("recent");
    const auto attempts = evidence.at("attempted").get<unsigned>();
    if (evidence.at("scope") != "one_new_stack_near_each_target" || !evidence.at("attempted").is_number_unsigned() ||
        attempts == 0 || attempts > 32 || attempts > args.at("count").get<unsigned>() || !recent.is_array() ||
        recent.size() != std::min(4u,attempts) || evidence.at("omitted") != attempts-recent.size())
        throw std::invalid_argument("MC: invalid pickup coverage");
    Json self = Json::object(), other = Json::object();
    const auto wanted = args.value("item",args.value("pickupItem",std::string{}));
    for (const auto& entry : recent) {
        const auto& target = entry.at("targetCell");
        for (const auto axis : {"x","y","z"}) if (!target.at(axis).is_number_integer() ||
            (args.contains(axis) && args.at(axis) != target.at(axis)))
            throw std::invalid_argument("MC: pickup target changed");
        const auto& state = entry.at("state");
        if (state == "unseen" && entry.size() == 2) continue;
        const auto& name = entry.at("item").get_ref<const std::string&>();
        const auto& seen = entry.at("seenAtMs");
        if ((state != "visible" && state != "left_view") || !entry.at("entityId").is_number_unsigned() ||
            (entry.contains("uuid") && !game_uuid(entry.at("uuid"))) || item_name(name) != name ||
            name.empty() || (!wanted.empty() && name != item_name(wanted)) ||
            !entry.at("seenCount").is_number_unsigned() || entry.at("seenCount") == 0 || !seen.is_number_unsigned() ||
            ((state == "left_view") != entry.contains("leftAtMs")) || (state == "left_view" &&
            (!entry.at("leftAtMs").is_number_unsigned() || entry.at("leftAtMs") < seen)))
            throw std::invalid_argument("MC: invalid pickup entity");
        const auto offset = [&](const char* axis) { return entry.at("position").at(axis).get<double>()-target.at(axis).get<double>()-0.5; };
        if (std::hypot(offset("x"),offset("y"),offset("z")) > 3.00000001)
            throw std::invalid_argument("MC: pickup entity outside observed radius");
        for (const auto key : {"selfCount","otherCount"}) if (!entry.at(key).is_number_unsigned() || entry.at(key) > 0x7fffffff)
            throw std::invalid_argument("MC: invalid collected quantity");
        const auto own = entry.at("selfCount").get<unsigned>(), others = entry.at("otherCount").get<unsigned>();
        if (own) self[name] = self.value(name,0ULL)+own;
        if (others) other[name] = other.value(name,0ULL)+others;
        if (entry.contains("lastCollector") != (own || others)) throw std::invalid_argument("MC: missing pickup event");
        if (entry.contains("lastCollector")) {
            const auto& last = entry.at("lastCollector");
            if (!last.at("entityId").is_number_unsigned() || (last.contains("uuid") && !game_uuid(last.at("uuid"))) ||
                !last.at("count").is_number_unsigned() || last.at("count") == 0 ||
                last.at("count") > (last.at("self").get<bool>() ? own : others) ||
                !last.at("sampledAtMs").is_number_unsigned() || last.at("sampledAtMs") < seen)
                throw std::invalid_argument("MC: invalid pickup event");
        }
    }
    const auto verify_totals = [&](const Json& total, const Json& sampled) {
        if (!total.is_object() || (evidence.at("omitted") == 0 && total != sampled))
            throw std::invalid_argument("MC: pickup totals differ from observed events");
        for (const auto& [item,count] : total.items()) if (item.empty() || item_name(item) != item ||
            (!wanted.empty() && item != item_name(wanted)) || !count.is_number_unsigned() || count == 0)
            throw std::invalid_argument("MC: invalid pickup totals");
        for (const auto& [item,count] : sampled.items()) if (!total.contains(item) || total.at(item) < count)
            throw std::invalid_argument("MC: pickup total lost observed events");
    };
    verify_totals(evidence.at("selfCollected"),self);
    verify_totals(evidence.at("otherCollected"),other);
}

// 核对采掘进展属于原目标，破坏数和当前库存各自有对应读数。
// 步骤：验证阶段与时点 → 检查原目标和破坏数量 → 对照取材要求与最终库存。
void verify_collect_progress(const Json& result, const Json& args, bool exited) {
    const auto& step = result.at("progress");
    const auto& phase = step.at("phase");
    const auto& sampled = step.at("sampledAtMs");
    const auto& broken = step.at("broken");
    const bool finished = phase == "finished";
    if ((phase != "selecting" && phase != "checking_target" && phase != "approaching" && phase != "preparing" && phase != "digging" &&
        phase != "awaiting_pickup" && !finished) || (finished && !exited) || (exited && result.at("status") == "done" && !finished) ||
        !sampled.is_number_unsigned() || !step.at("phaseSinceMs").is_number_unsigned() || step.at("phaseSinceMs") > sampled ||
        !broken.is_number_unsigned() || broken != result.at("broken") || broken > args.at("count") ||
        ((broken != 0) != step.contains("lastBreakAtMs")) || (phase == "awaiting_pickup" && broken == 0) ||
        (phase == "checking_target" && broken >= args.at("count")) ||
        (step.contains("lastBreakAtMs") && (!step.at("lastBreakAtMs").is_number_unsigned() || step.at("lastBreakAtMs") > sampled)))
        throw std::invalid_argument("MC: invalid collect progress");

    const bool has_target = step.contains("targetCell");
    if (has_target != step.contains("block") || (phase == "selecting" && has_target) ||
        (phase != "selecting" && !finished && !has_target))
        throw std::invalid_argument("MC: missing or stale collect target");
    if (has_target) {
        if (!dimension(step.at("block")) || (args.contains("block") &&
            step.at("block").get_ref<const std::string&>() != item_name(args.at("block").get_ref<const std::string&>())))
            throw std::invalid_argument("MC: collect progress material changed");
        for (const auto axis : {"x","y","z"}) if (!step.at("targetCell").at(axis).is_number_integer() ||
            (args.contains(axis) && step.at("targetCell").at(axis) != args.at(axis)))
            throw std::invalid_argument("MC: collect progress target changed");
    }
    if (step.contains("pickup")) {
        const auto& recent = result.at("pickups").at("recent");
        if (phase != "awaiting_pickup" || recent.empty() || recent.back().at("targetCell") != step.at("targetCell"))
            throw std::invalid_argument("MC: pickup progress target changed");
        Json summary = Json::object();
        for (const auto key : {"state","entityId","item","selfCount","otherCount","lastCollector"})
            if (recent.back().contains(key)) summary[key] = recent.back().at(key);
        if (step.at("pickup") != summary) throw std::invalid_argument("MC: stale pickup progress");
    }
    if (step.contains("stock") != args.contains("item")) throw std::invalid_argument("MC: collect progress stock missing");
    if (step.contains("stock")) {
        const auto& stock = step.at("stock");
        if (stock.at("item").get_ref<const std::string&>() != item_name(args.at("item").get_ref<const std::string&>()) ||
            stock.at("required") != args.at("requiredStock") || !stock.at("current").is_number_unsigned() ||
            (exited && stock.at("current") != result.at("stock").at("after")))
            throw std::invalid_argument("MC: inconsistent collect stock");
    }
}

// 核对移动样本的原目标、现场读数、寻路状态及末次真实抵达。
// 步骤：校验时点与阶段 → 对照原命令的目标格 → 核对已知位置与实际距离。
void verify_move_progress(const Json& result, const Json& args, bool exited) {
    const auto& step = result.at("progress");
    const auto& phase = step.at("phase");
    const auto& path = step.at("path");
    const auto& time = step.at("sampledAtMs");
    const auto& target = step.at("targetCell");
    if ((phase != "waiting" && phase != "moving" && phase != "unknown" && phase != "no_path" && phase != "arrived") ||
        (path != "unknown" && path != "partial" && path != "success" && path != "noPath" && path != "timeout") ||
        !time.is_number_unsigned() || time > 9007199254740991ULL ||
        (step.contains("lastMovedAtMs") && (!step.at("lastMovedAtMs").is_number_unsigned() || step.at("lastMovedAtMs") > time)) ||
        (phase == "moving" && !step.contains("lastMovedAtMs")) || (phase == "no_path" && path != "noPath") ||
        ((phase == "arrived") != (exited && result.at("status") == "done")) ||
        (phase == "arrived" && (!result.value("arrived",false) || step.at("position") != result.at("position"))))
        throw std::invalid_argument("MC: invalid move progress");
    for (const auto axis : {"x","y","z"}) {
        if (!target.at(axis).is_number_integer() || (args.contains(axis) && args.at(axis) != target.at(axis)))
            throw std::invalid_argument("MC: move progress target changed");
    }
    const bool known = phase != "unknown";
    if (known != step.contains("position") || known != step.contains("distance") || (!known && step.contains("vertical")))
        throw std::invalid_argument("MC: inconsistent move position");
    if (known) {
        const auto& reported = step.at("distance");
        if (!reported.is_number() || !std::isfinite(reported.get<double>()) || reported < 0)
            throw std::invalid_argument("MC: invalid move distance");
        const auto& position = step.at("position");
        for (const auto axis : {"x","y","z"}) if (!position.at(axis).is_number() ||
            !std::isfinite(position.at(axis).get<double>())) throw std::invalid_argument("MC: invalid move position");
        const auto offset = [&](const char* axis) { return position.at(axis).get<double>() - target.at(axis).get<double>(); };
        const double distance = std::hypot(offset("x")-0.5,offset("y"),offset("z")-0.5);
        if (!std::isfinite(distance) || std::abs(reported.get<double>()-distance) > 1e-8 * std::max(1.0,distance))
            throw std::invalid_argument("MC: move distance does not match sampled position");
        if (step.contains("vertical") && step.at("vertical") != -offset("y"))
            throw std::invalid_argument("MC: move height does not match sampled position");
        if (step.contains("startDistance") && (!step.at("startDistance").is_number() ||
            !std::isfinite(step.at("startDistance").get<double>()) || step.at("startDistance") < 0))
            throw std::invalid_argument("MC: invalid starting distance");
    }
}

// 按原路线核对有序的实际脚位和圈数，允许失败或取消时返回已完成的前缀。
// 步骤：核对工作参数 → 验证前缀实际站位 → 核对进度和终态。
void verify_route(const Json& result, const Json& args, bool exited) {
    const auto requested = args.at("laps").get<unsigned>();
    const auto& points = args.at("waypoints");
    if (points.size() != 4 || requested < 1 || requested > 8)
        throw std::invalid_argument("MC: invalid checked route parameters");
    if (!result.contains("route")) {
        if (result.contains("progress") || (exited && result.at("status") == "done"))
            throw std::invalid_argument("MC: missing route checkpoints");
        return;
    }
    const auto& route = result.at("route");
    const auto& checkpoints = route.at("checkpoints");
    if (route.at("id") != args.at("route") || route.at("requested") != requested ||
        !checkpoints.is_array() || checkpoints.size() > requested*4+1 ||
        (exited && result.at("status") == "done" && checkpoints.size() != requested*4+1))
        throw std::invalid_argument("MC: invalid route settlement");
    for (std::size_t index = 0; index < checkpoints.size(); ++index) {
        const auto& checkpoint = checkpoints.at(index);
        if (checkpoint.at("index") != index) throw std::invalid_argument("MC: unordered route checkpoints");
        for (const auto axis : {"x","y","z"}) {
            const auto& position = checkpoint.at("position").at(axis);
            if (!position.is_number() || std::floor(position.get<double>()) != points.at(index%4).at(axis).get<double>())
                throw std::invalid_argument("MC: unchecked route position");
        }
    }
    const auto index = checkpoints.empty() ? 0 : checkpoints.size()-1;
    if (route.at("laps") != index/4) throw std::invalid_argument("MC: inconsistent route count");
    if (checkpoints.empty()) {
        if (result.contains("progress")) throw std::invalid_argument("MC: route progress without checkpoint");
    } else if (const auto& step = result.at("progress"); step.at("phase") != "route" ||
        step.at("requested") != requested || step.at("checkpoint") != index || step.at("laps") != index/4)
        throw std::invalid_argument("MC: inconsistent route progress");
}
}

// 解析宿主信封，为提交、重读和停止附上同一控制实例与原参数。
std::string partner_request(std::string_view method, std::string_view id, const BridgeConfig& config,
    std::string_view skill, std::string_view arguments) {
    auto envelope = parse(arguments);
    auto& context = envelope.at("context");
    if (config.key.size() != 64 || config.controller.size() != 32 || envelope.size() != 2 ||
        !envelope.at("arguments").is_object() || context.size() != 2 ||
        !game_uuid(context.at("generation")) || !context.at("order").is_number_unsigned() || context.at("order") == 0 ||
        context.at("order") > 9007199254740991ULL) throw std::invalid_argument("MC: invalid partner context");
    if (skill == "pickup_drop" && !valid_pickup_request(envelope.at("arguments")))
        throw std::invalid_argument("MC: invalid ground pickup arguments");
    context["controller"] = config.controller;
    // 旧桥接会拒绝受约束的技能名，防止忽略新审批参数后执行采掘。
    const auto wire_skill = skill == "collect" && envelope.at("arguments").contains("collectionGuards") ? "guarded_collect" : skill;
    return Json{{"version",1},{"id",id},{"key",config.key},{"method",method},{"context",std::move(context)},
        {"skill",wire_skill},{"arguments",std::move(envelope.at("arguments"))}}.dump();
}
// 对照原技能和宿主来源读取实际结算，业务受阻也作为信息返回给模型。
PartnerReply partner_reply(std::string_view wire, std::string_view id, std::string_view controller,
    std::string_view skill, std::string_view arguments) {
    const auto frame = parse(wire,skill == "observe" || skill == "route" || skill == "collect" || skill == "pickup_drop" ? 12 : 6), envelope = parse(arguments);
    const auto& context = envelope.at("context");
    const auto& args = envelope.at("arguments");
    const auto& action = frame.at("action");
    if (frame.at("version") != 1 || frame.at("id").get_ref<const std::string&>() != id ||
        action.at("controller").get_ref<const std::string&>() != controller ||
        action.at("skill").get_ref<const std::string&>() != skill || action.at("generation") != context.at("generation") ||
        action.at("order") != context.at("order") || !action.at("exited").is_boolean() || !action.at("result").is_object())
        throw std::invalid_argument("MC: invalid partner receipt");
    const bool exited = action.at("exited");
    const auto& result = action.at("result");
    if (skill == "observe" && (result.contains("dropTarget") ||
        (exited && result.at("status") == "done" && args.value("mode","") == "drop"))) {
        if (args.value("mode","") != "drop") throw std::invalid_argument("MC: unexpected drop reference");
        verify_drop_target(result.at("dropTarget"),args,context.at("generation"));
    }
    if (skill == "observe" && (result.contains("dropObservation") ||
        (exited && result.at("status") == "done" && args.value("mode","") == "drops")))
        verify_drop_observation(result.at("dropObservation"),id,context.at("generation"),args);
    if (skill == "observe" && result.contains("observation")) {
        const auto& observation = result.at("observation");
        const auto& samples = observation.at("samples");
        const auto requested = observation.at("requested").get<unsigned>();
        if (observation.at("id").get_ref<const std::string&>() != id || observation.at("generation") != context.at("generation") ||
            !dimension(observation.at("dimension")) || requested < 1 || requested > 3 ||
            !samples.is_array() || samples.size() > requested)
            throw std::invalid_argument("MC: invalid observation series");
    }
    if (skill == "observe" && result.contains("openingObservation")) {
        const auto& source = result.at("openingObservation");
        if (source.at("id").get_ref<const std::string&>() != id || source.at("message") != args.at("message") || source.at("generation") != context.at("generation") ||
            !dimension(source.at("dimension")) || source.at("dimension") != result.at("dimension"))
            throw std::invalid_argument("MC: invalid opening observation");
    }
    if (skill == "observe" && result.contains("sourceObservation")) {
        const auto& source = result.at("sourceObservation");
        if (args.value("mode","") != "detail" || source.at("id") != args.at("observation") ||
            source.at("generation") != context.at("generation") || !dimension(source.at("dimension")) || !result.contains("survey"))
            throw std::invalid_argument("MC: invalid terrain detail source");
    }
    if (skill == "observe" && result.contains("route")) {
        const auto& route = result.at("route");
        if (args.value("mode","") != "route" || (route.at("status") == "validated" &&
            (route.at("id").get_ref<const std::string&>() != id || route.at("generation") != context.at("generation") ||
             route.at("observation") != args.at("observation") || !dimension(route.at("dimension")) ||
             !route.at("sourceMessage").is_number_unsigned() || !route.at("waypoints").is_array() ||
             route.at("waypoints").size() != 4))) throw std::invalid_argument("MC: invalid route observation");
    }
    if (exited && result.at("status") != "done" && result.at("status") != "blocked" && result.at("status") != "interrupted")
        throw std::invalid_argument("MC: invalid partner outcome");
    auto delivery = ReplyDelivery::unknown;
    if (exited && skill == "reply") {
        const bool sent = result.value("sent",false);
        const auto state = result.value("delivery",sent ? "sent" : "unknown");
        const bool reused = result.value("reused",false);
        if ((state != "sent" && state != "not_sent" && state != "unknown") || sent != (state == "sent") ||
            (sent && result.at("text") != args.at("text")) ||
            (state == "not_sent" && (result.at("status") == "done" || reused)))
            throw std::invalid_argument("MC: inconsistent reply delivery");
        if (sent) delivery = ReplyDelivery::sent;
        else if (state == "not_sent") delivery = ReplyDelivery::not_sent;
    }
    if (exited && skill == "move" && args.value("exact",false) && result.at("status") == "done") {
        if (result.at("arrived") != true) throw std::invalid_argument("MC: exact arrival not verified");
        const auto& position = result.at("position");
        for (const auto axis : {"x","y","z"})
            if (!position.at(axis).is_number() || std::floor(position.at(axis).get<double>()) != args.at(axis).get<double>())
                throw std::invalid_argument("MC: arrived at a different cell");
    }
    bool attempted = false, verified = false;
    if (exited && skill == "place") {
        attempted = result.at("placementAttempted").get<bool>();
        verified = result.at("placed").get<bool>();
        if ((verified && !attempted) || (!verified && result.at("status") == "done"))
            throw std::invalid_argument("MC: inconsistent placement status");
        if (verified) verify_placement(result,envelope.at("arguments"));
    }
    if (exited && skill == "fill_target") {
        attempted = result.at("placementAttempted").get<bool>();
        verify_fill(result,envelope.at("arguments"),result.at("status") == "done");
    }
    std::string progress;
    const bool picked = skill == "pickup_drop" && verify_ground_pickup(result,args,context.at("generation"),exited);
    if (skill == "pickup_drop" && result.contains("progress")) progress = result.at("progress").dump();
    if (skill == "collect" && (result.contains("capacity") || result.value("reasonCode","") == "pickup_inventory_full")) {
        const auto& capacity = result.at("capacity");
        verify_inventory_capacity(capacity,item_name(args.at("item").get_ref<const std::string&>()));
        if (result.value("reasonCode","") == "pickup_inventory_full" && (capacity.at("status") != "sampled" ||
            capacity.at("knownSpace") != 0 || !exited || result.at("status") != "blocked"))
            throw std::invalid_argument("MC: unverified full inventory");
    }
    if (skill == "collect" && result.contains("pickups")) verify_collect_pickups(result.at("pickups"),args);
    if (skill == "collect" && result.contains("progress")) {
        verify_collect_progress(result,args,exited);
        progress = result.at("progress").dump();
    }
    if (skill == "move" && result.contains("progress")) {
        verify_move_progress(result,args,exited);
        progress = result.at("progress").dump();
    }
    if (skill == "follow" && result.contains("progress")) {
        const auto& step = result.at("progress");
        const auto name = step.at("phase").get<std::string>();
        const bool waiting = name == "waiting_player";
        const auto terrain = step.value("terrain",std::string("land"));
        const auto path = step.value("path",std::string("unknown"));
        if (step.size() != (waiting ? 1u : 2u) + step.count("terrain") + step.count("vertical") + step.count("path") +
            step.count("sampledAtMs") ||
            (terrain != "land" && terrain != "swimming" && terrain != "leaving_water" && terrain != "dry") ||
            (path != "unknown" && path != "reachable" && path != "unreachable") ||
            (step.contains("vertical") && (!step.at("vertical").is_number() ||
                std::abs(step.at("vertical").get<double>()) > 60000000)) ||
            (step.contains("sampledAtMs") && (!step.at("sampledAtMs").is_number_unsigned() ||
                step.at("sampledAtMs") > 9007199254740991ULL)) ||
            (!waiting && (name != "in_range" && name != "following" && name != "replanning" && name != "blocked")) ||
            (!waiting && (!step.at("distance").is_number() || !std::isfinite(step.at("distance").get<double>()) ||
                step.at("distance") < 0))) throw std::invalid_argument("MC: invalid follow progress");
        progress = step.dump();
    }
    if (skill == "route") {
        verify_route(result,args,exited);
        if (result.contains("progress")) progress = result.at("progress").dump();
    }
    const bool craft_attempted = exited && skill == "craft" && result.at("craftAttempted").get<bool>();
    const bool changed = exited && (skill == "pickup_drop" ? picked : skill == "craft" ? verified_craft(result,args,craft_attempted) :
        result.contains("changes") && verified_changes(result.at("changes")));
    return {exited, exited && result.at("status") == "done", attempted, verified, changed, delivery, exited ? result.dump() : std::string{},
        std::move(progress), craft_attempted, skill == "pickup_drop" && result.contains("pickup") && result.at("pickup").at("approachAttempted") == true};
}
// 将普通退出和显式接管分别绑定到控制实例或原游戏世界。
// 步骤：核对目标范围 → 选择释放或接管方法 → 生成原请求帧。
std::string control_request(std::string_view id, std::string_view key, std::string_view arguments) {
    auto frame = parse(arguments);
    const auto scoped = frame.contains("controller");
    const auto& generation = frame.at("generation");
    if (key.size() != 64 || frame.size() != (scoped ? 2u : 1u) || (!game_uuid(generation) && !(scoped && generation == "")) ||
        (scoped && !controller_id(frame.at("controller").get_ref<const std::string&>()))) throw std::invalid_argument("MC: stop identity");
    frame.update(Json{{"version", 1}, {"id", id}, {"key", key}, {"method", scoped ? "control.detach" : "control.stop"}});
    if (!scoped) frame["scope"] = "role";
    return frame.dump();
}
// 只让原请求关联到对应代次的实际释放，待退出的回执留给原票据继续轮询。
std::string control_reply(std::string_view wire, std::string_view id, std::string_view arguments) {
    const auto reply = parse(wire), query = parse(arguments);
    if (reply.size() != 3 || reply.at("id").get_ref<const std::string&>() != id || reply.at("version") != 1 ||
        !control_receipt(reply.at("control"), query.at("generation"), query.value("controller",Json{})))
        throw std::invalid_argument("MC: stop reply");
    const auto& value = reply.at("control");
    return value.at("phase") == "requested" ? std::string{} : value.dump();
}

// 读取插件的独立通道、可选私有配对键和玩家回合的思考偏好。
// 步骤：核对配置字段 → 校验配对键 → 解析玩家回合模式并签发控制身份。
BridgeConfig configured_bridge(std::string_view config) {
    const auto value = parse(config);
    const auto channel = value.at("channel").get<std::string>();
    if (!valid_channel(channel) || value.size() != 1 + value.count("bridgeKey") + value.count("playerThinking"))
        throw std::invalid_argument("MC protocol: channel");
    std::string key;
    if (value.contains("bridgeKey")) {
        key = value.at("bridgeKey").get<std::string>();
        if (key.size() != 64 || key.find_first_not_of("0123456789abcdef") != key.npos)
            throw std::invalid_argument("MC protocol: invalid private configuration");
    }
    const auto mode = value.value("playerThinking",std::string("inherit"));
    if (mode != "inherit" && mode != "off" && mode != "low") throw std::invalid_argument("MC protocol: playerThinking");
    return {channel,std::move(key),new_controller(),
        mode == "low" ? PlayerThinking::low : mode == "off" ? PlayerThinking::off : PlayerThinking::inherit};
}
// 生成独立于工作序号的控制周期身份。
std::string new_controller() {
    std::random_device random;
    return std::format("{:08x}{:08x}{:08x}{:08x}",random(),random(),random(),random());
}
// 仅取公开通道名。
std::string configured_channel(std::string_view config) { return configured_bridge(config).channel; }

// 在工具层生成查询，模型只能提供空参数。
std::string world_request(std::string_view id) {
    return Json{{"version", 1}, {"method", "state.read"}, {"id", id}}.dump();
}

// 判断在场事实的来源游标与种类，不从文本推断玩家状态。
static bool presence_fact(const Json& value) {
    return value.is_object() && value.size() == 2 && value.contains("sequence") &&
        value.at("sequence").is_number_unsigned() && value.at("sequence") != 0 &&
        (value.value("type", "") == "player.entered" || value.value("type", "") == "player.left");
}

// 校验原话现场的采样时间、维度和可选玩家坐标。
static bool message_scene(const Json& value) {
    if (!value.is_object() || value.size() != 3u + value.count("facing") + value.count("eye") || !dimension(value.at("dimension")) ||
        !value.at("sampledAtMs").is_number_unsigned() || value.at("sampledAtMs") > 9007199254740991ULL) return false;
    const auto& position = value.at("position");
    if (position.is_null()) return !value.contains("facing") && !value.contains("eye");
    if (!position.is_object() || position.size() != 3) return false;
    for (const auto* axis : {"x","y","z"})
        if (!position.at(axis).is_number() || !std::isfinite(position.at(axis).get<double>())) return false;
    if (value.contains("facing")) {
        const auto& facing = value.at("facing");
        if (!facing.is_object() || facing.size() != 2) return false;
        for (const auto* angle : {"yaw","pitch"})
            if (!facing.at(angle).is_number() || !std::isfinite(facing.at(angle).get<double>())) return false;
    }
    if (value.contains("eye")) {
        const auto& eye = value.at("eye");
        if (!value.contains("facing") || !eye.is_object() || eye.size() != 3 || eye.at("x") != position.at("x") ||
            eye.at("z") != position.at("z") || !eye.at("y").is_number()) return false;
        const auto height = eye.at("y").get<double>()-position.at("y").get<double>();
        if (!(height > 0 && height <= 2)) return false;
    }
    return true;
}
// 校验原查询对应的世界观测，继续保留客户端数量和服务端信号年龄的语义。
// 步骤：核对信封和身份 → 处理已撤销世界 → 校验字段类型与范围并返回值副本。
std::string world_reply(std::string_view wire, std::string_view expected_id, bool with_survey) {
    const auto reply = parse(wire);
    if (reply.size() != 3 || !reply.at("version").is_number_unsigned() ||
        reply.at("version") != 1 || !reply.at("id").is_string() || reply.at("id").get_ref<const std::string&>() != expected_id)
        throw std::invalid_argument("MC protocol: wrong reply");
    const auto& state = reply.at("state");
    if (state.is_null()) return "null";

    const bool has_survey = with_survey && state.contains("survey");
    const bool has_answer = with_survey && state.contains("answer");
    if (state.size() != 6u + has_survey + has_answer || !game_uuid(state.at("generation")) || !game_uuid(state.at("actor")) ||
        !dimension(state.at("dimension")) || !state.at("cobblestone").is_number_unsigned() || state.at("cobblestone") > 9007199254740991ULL ||
        !state.at("signalAgeMs").is_number_unsigned() || state.at("signalAgeMs") > 9007199254740991ULL)
        throw std::invalid_argument("MC protocol: wrong world state");
    const auto& pos = state.at("position");
    if (pos.size() != 3) throw std::invalid_argument("MC protocol: wrong position");
    for (const auto* key : {"x", "y", "z"})
        if (!pos.at(key).is_number() || !std::isfinite(pos.at(key).get<double>()))
            throw std::invalid_argument("MC protocol: wrong coordinate");
    if (has_survey) {
        const auto& s = state.at("survey");
        if (s.size() != 7 || !s.at("area").is_string() || !s.at("returns").is_array() || s.at("returns").size() != 3)
            throw std::invalid_argument("MC protocol: invalid return window");
        const auto& area = s.at("area").get_ref<const std::string&>();
        if (area.empty() || area.size() > 48 || area.find_first_not_of("0123456789-,") != area.npos)
            throw std::invalid_argument("MC protocol: invalid survey area");
        for (auto field : {"order", "window", "sample", "gained", "seen"})
            if (!s.at(field).is_number_unsigned() || s.at(field) > 9007199254740991ULL)
                throw std::invalid_argument("MC protocol: survey counter");
        std::uint64_t last = 0;
        for (const auto& id : s.at("returns")) {
            if (!id.is_number_unsigned() || id <= last || id > s.at("sample")) throw std::invalid_argument("MC protocol: return order");
            last = id.get<std::uint64_t>();
        }
        if (s.at("order") == 0 || s.at("window") == 0 || s.at("gained") > s.at("seen") || s.at("seen") > 256)
            throw std::invalid_argument("MC protocol: survey coverage");
    }
    if (has_answer) {
        const auto& a = state.at("answer");
        if (a.size() != 3 || !a.at("order").is_number_unsigned() || a.at("order") == 0 ||
            !inquiry_id(a.at("inquiry")) ||
            !a.at("text").is_string() || a.at("text").get_ref<const std::string&>().empty() ||
            a.at("text").get_ref<const std::string&>().size() > 512) throw std::invalid_argument("MC protocol: route answer");
    }
    return state.dump();
}

// 只有配置过配对键的原生实例可以请求角色的探索摘要。
std::string survey_request(std::string_view id, std::string_view key, std::string_view arguments) {
    auto frame = parse(arguments);
    if (key.size() != 64 || !frame.is_object() || (!frame.empty() && (frame.size() != 2 || !frame.at("order").is_number_unsigned() ||
        frame.at("order") == 0 || frame.at("order") > 9007199254740991ULL || !inquiry_id(frame.at("inquiry")))))
        throw std::invalid_argument("MC protocol: route inquiry");
    frame.update(Json{{"version", 1}, {"method", "survey.read"}, {"id", id}, {"key", key}});
    return frame.dump();
}

// 从受控参数建立私有收件或伙伴监听请求。
// 步骤：核对原世界及游标 → 按有无控制实例选择真实读口。
std::string inbox_request(std::string_view id, std::string_view key, std::string_view arguments, std::string_view controller) {
    auto request = parse(arguments);
    if (key.size() != 64 || request.size() != 2 || !game_uuid(request.at("generation")) ||
        !request.at("after").is_number_unsigned() || request.at("after") > 9007199254740991ULL ||
        (!controller.empty() && !controller_id(controller)))
        throw std::invalid_argument("MC protocol: invalid inbox query");
    request.update(Json{{"version", 1}, {"id", id}, {"key", key}, {"method", controller.empty() ? "chat.read" : "partner.listen"}});
    if (!controller.empty()) request["controller"] = controller;
    return request.dump();
}

// 校验世界、绑定玩家、窗口缺口和每条原话的序号。
// 步骤：匹配原请求 → 验证保留窗口 → 检查消息数量、文本与续读标记。
std::string inbox_reply(std::string_view wire, std::string_view id, std::string_view arguments) {
    const auto reply = parse(wire), query = parse(arguments);
    if (reply.size() != 3 || !reply.at("version").is_number_unsigned() || reply.at("version") != 1 ||
        reply.at("id").get_ref<const std::string&>() != id) throw std::invalid_argument("MC protocol: wrong private reply");
    const auto& inbox = reply.at("inbox");
    if (inbox.is_null()) return "null";
    if (inbox.size() != 7u + inbox.count("control") + inbox.count("presence") + inbox.count("sceneRevision") +
        inbox.count("lastDeparture") + inbox.count("execution") ||
        inbox.at("generation") != query.at("generation") || !game_uuid(inbox.at("owner")) ||
        !inbox.at("oldest").is_number_unsigned() || !inbox.at("latest").is_number_unsigned() ||
        !inbox.at("gap").is_boolean() || !inbox.at("more").is_boolean()) throw std::invalid_argument("MC protocol: inbox shape");
    const auto oldest = inbox.at("oldest").get<std::uint64_t>(), latest = inbox.at("latest").get<std::uint64_t>();
    const auto after = query.at("after").get<std::uint64_t>();
    if (inbox.contains("presence") && (!presence_fact(inbox.at("presence")) || inbox.at("presence").at("sequence") > latest))
        throw std::invalid_argument("MC protocol: invalid player presence");
    if (inbox.contains("lastDeparture")) {
        const auto& departure = inbox.at("lastDeparture");
        if (!departure.is_number_unsigned() || departure > latest || (departure != 0 && !inbox.contains("presence")))
            throw std::invalid_argument("MC protocol: invalid player departure");
        if (inbox.contains("presence")) {
            const auto& presence = inbox.at("presence");
            if ((presence.at("type") == "player.left" && departure != presence.at("sequence")) ||
                (presence.at("type") == "player.entered" && departure >= presence.at("sequence")))
                throw std::invalid_argument("MC protocol: inconsistent player departure");
        }
    }
    if (inbox.contains("control") && (!control_receipt(inbox.at("control"), query.at("generation")) ||
        inbox.at("control").at("order") > latest)) throw std::invalid_argument("MC: stop cursor");
    if (inbox.contains("execution")) {
        const auto& execution = inbox.at("execution");
        const auto& mode = execution.at("mode");
        const auto& state = execution.at("state");
        if (execution.size() != 2 || (mode != "partner" && mode != "scenario" && mode != "none") ||
            (state != "ready" && state != "busy" && state != "requested" && state != "released" && state != "unconfirmed" && state != "unavailable") ||
            (state == "busy" && mode != "partner") ||
            (mode == "none" && state != "unavailable") || (inbox.contains("control") && state != inbox.at("control").at("phase")))
            throw std::invalid_argument("MC protocol: invalid execution status");
    }
    if (inbox.contains("sceneRevision") && (!inbox.at("sceneRevision").is_number_unsigned() ||
        inbox.at("sceneRevision") == 0 || inbox.at("sceneRevision") > 9007199254740991ULL))
        throw std::invalid_argument("MC: invalid scene revision");
    if (latest > 9007199254740991ULL || !oldest || (latest ? oldest > latest : oldest != 1) || after > latest ||
        inbox.at("gap") != (after < oldest - 1)) throw std::invalid_argument("MC protocol: inbox cursor");

    const auto& messages = inbox.at("messages");
    if (!messages.is_array() || messages.size() > 8) throw std::invalid_argument("MC protocol: inbox size");
    auto cursor = std::max(after, oldest - 1);
    for (const auto& message : messages) {
        if (!message.at("sequence").is_number_unsigned() || message.at("sequence") != ++cursor)
            throw std::invalid_argument("MC protocol: event sequence");
        if (inbox.contains("lastDeparture")) {
            const bool left = message.value("type","") == "player.left";
            const auto& departure = inbox.at("lastDeparture");
            if ((left && cursor > departure) || (!left && cursor == departure))
                throw std::invalid_argument("MC protocol: departure cursor does not match the event");
        }
        if (presence_fact(message)) {
            if (!inbox.contains("presence") || cursor > inbox.at("presence").at("sequence") ||
                (cursor == inbox.at("presence").at("sequence") && message != inbox.at("presence")))
                throw std::invalid_argument("MC protocol: inconsistent player presence");
            continue;
        }
        if (inbox.contains("presence") && cursor == inbox.at("presence").at("sequence"))
            throw std::invalid_argument("MC protocol: player speech at presence cursor");
        if (message.size() != 2u + message.count("offer") + message.count("type") + message.count("sceneAtMessage") ||
            message.value("type", "player.message") != "player.message" || !message.at("text").is_string())
            throw std::invalid_argument("MC protocol: inbox message");
        if (message.contains("sceneAtMessage") && !message_scene(message.at("sceneAtMessage")))
            throw std::invalid_argument("MC protocol: invalid scene at message");
        const auto& text = message.at("text").get_ref<const std::string&>();
        if (text.empty() || text.size() > 1024) throw std::invalid_argument("MC protocol: inbox text");
        if (message.contains("offer")) {
            const auto& offer = message.at("offer");
            const auto& purpose = offer.at("purpose");
            if (offer.size() == 2 && purpose == "visit.inspect" && valid_channel(offer.at("place").get_ref<const std::string&>())) continue;
            if (offer.size() == 2 && purpose == "question.decline" && valid_channel(offer.at("question").get_ref<const std::string&>())) continue;
            if (offer.size() == 3 && purpose == "collect.return" && valid_channel(offer.at("question").get_ref<const std::string&>()) &&
                offer.at("quantity").is_number_unsigned() && offer.at("quantity") > 0 && offer.at("quantity") <= 64) continue;
            throw std::invalid_argument("MC protocol: inbox offer");
        }
    }
    if (cursor > latest || (cursor < latest && messages.empty()) || inbox.at("more") != (cursor < latest))
        throw std::invalid_argument("MC protocol: inbox continuation");
    return inbox.dump();
}

// 为同一逻辑动作生成接纳、查询或停止帧，不在参数中接收私有键或方法。
std::string inspect_request(std::string_view method, std::string_view id, std::string_view key, std::string_view arguments) {
    auto frame = parse(arguments);
    if (key.size() != 64 || frame.size() != 3 || !game_uuid(frame.at("generation")) || !frame.at("order").is_number_unsigned() ||
        frame.at("order") == 0 || frame.at("order") > 9007199254740991ULL || !frame.at("place").is_string() ||
        !valid_channel(frame.at("place").get_ref<const std::string&>())) throw std::invalid_argument("MC protocol: inspect target");
    if (method != "inspect.begin") {
        if (method != "inspect.read" && method != "inspect.stop") throw std::invalid_argument("MC protocol: inspect control");
        frame.erase("place");
    }
    frame.update(Json{{"version", 1}, {"id", id}, {"key", key}, {"method", method}});
    return frame.dump();
}

// 只接受与原委托一致的动作状态，到达时石料可以是明确未看见的 null。
// 步骤：核对请求与目标 → 校验状态及退出语义 → 验证位置和可见方块读回。
InspectReply inspect_reply(std::string_view wire, std::string_view id, std::string_view arguments) {
    const auto reply = parse(wire), args = parse(arguments);
    if (reply.size() != 3 || !reply.at("version").is_number_unsigned() || reply.at("version") != 1 ||
        reply.at("id").get_ref<const std::string&>() != id) throw std::invalid_argument("MC protocol: inspect reply");
    const auto& action = reply.at("action");
    if (action.at("generation") != args.at("generation") || !action.at("order").is_number_unsigned() ||
        action.at("order") != args.at("order") || action.at("place") != args.at("place") ||
        !action.at("exited").is_boolean() || !action.at("moved").is_boolean()) throw std::invalid_argument("MC protocol: wrong action");
    const auto& phase = action.at("phase").get_ref<const std::string&>();
    const bool exited = action.at("exited"), arrived = phase == "arrived";
    if ((exited && !arrived && phase != "stopped" && phase != "failed") ||
        (!exited && phase != "moving" && phase != "stopping") || action.size() != (arrived ? 8u : 6u))
        throw std::invalid_argument("MC protocol: inconsistent action phase");
    if (arrived) {
        const auto& position = action.at("position");
        const auto& stone = action.at("stone");
        if (position.size() != 3 || (!stone.is_null() && stone.size() != 3)) throw std::invalid_argument("MC protocol: observation");
        for (auto axis : {"x", "y", "z"}) {
            if (!position.at(axis).is_number() || !std::isfinite(position.at(axis).get<double>()) ||
                (!stone.is_null() && !stone.at(axis).is_number_integer())) throw std::invalid_argument("MC protocol: observation coordinates");
        }
    }
    auto evidence = action;
    if (arrived) evidence["observed"] = !action.at("stone").is_null();
    return {exited, arrived, action.at("moved").get<bool>(), evidence.dump()};
}

// 仅用原委托和固定问题号请求游戏侧提问。
std::string question_request(std::string_view id, std::string_view key, std::string_view arguments) {
    auto value = parse(arguments);
    if (key.size() != 64 || value.size() != 3 || !game_uuid(value.at("generation")) || !value.at("order").is_number_unsigned() ||
        value.at("order") == 0 || value.at("order") > 9007199254740991ULL || !value.at("question").is_string() ||
        !valid_channel(value.at("question").get_ref<const std::string&>())) throw std::invalid_argument("MC protocol: invalid question");
    value.update(Json{{"version", 1}, {"id", id}, {"key", key}, {"method", "question.open"}});
    return value.dump();
}

// 核对本次世界、玩家、数量与会合区后生成有界请求。
std::string delivery_request(bool admit, std::string_view id, std::string_view key, std::string_view arguments) {
    auto grant = parse(arguments);
    const auto decimal = [&](const char* field) {
        const auto& text = grant.at(field).get_ref<const std::string&>();
        return !text.empty() && text.size() <= 20 && text[0] != '0' && text.find_first_not_of("0123456789") == text.npos;
    };
    if (key.size() != 64 || grant.size() != 10 || !decimal("question") || !decimal("grant") || !decimal("command") ||
        !game_uuid(grant.at("generation")) || !game_uuid(grant.at("owner")) || grant.at("material") != "minecraft:cobblestone" ||
        !valid_channel(grant.at("returnPlace").get_ref<const std::string&>()) ||
        !grant.at("quantity").is_number_unsigned() || grant.at("quantity") == 0 || grant.at("quantity") > 64 ||
        !grant.at("order").is_number_unsigned() || grant.at("order") == 0 || grant.at("order") > 9007199254740991ULL ||
        (grant.at("via") != "game" && grant.at("via") != "management")) throw std::invalid_argument("MC protocol: invalid collection grant");
    Json frame{{"version", 1}, {"id", id}, {"key", key}, {"method", admit ? "collection.admit" : "collection.receipt"}};
    if (admit) frame["grant"] = std::move(grant);
    else for (auto name : {"generation", "question", "grant"}) frame[name] = grant.at(name);
    return frame.dump();
}

// 核对送达的是原范围，查询空值不会生成新的接纳凭据。
std::string delivery_reply(bool admit, std::string_view wire, std::string_view id, std::string_view arguments) {
    const auto reply = parse(wire);
    if (reply.size() != 3 || !reply.at("version").is_number_unsigned() || reply.at("version") != 1 ||
        reply.at("id").get_ref<const std::string&>() != id) throw std::invalid_argument("MC protocol: wrong collection reply");
    const auto& receipt = reply.at("delivery");
    if (receipt.is_null()) {
        if (admit) throw std::invalid_argument("MC protocol: missing admission");
        return "null";
    }
    auto expected = parse(arguments);
    if (receipt.at("phase") == "unreceived") {
        if (admit || receipt.size() != 4) throw std::invalid_argument("MC protocol: wrong absence receipt");
        for (auto name : {"generation", "question", "grant"}) if (receipt.at(name) != expected.at(name))
            throw std::invalid_argument("MC protocol: wrong absence identity");
    } else {
        expected["phase"] = "accepted";
        if (receipt != expected) throw std::invalid_argument("MC protocol: changed collection scope");
    }
    return receipt.dump();
}

// 只为原世界、问题、委托和有界块编号建立采掘控制帧。
std::string mine_request(std::string_view method, std::string_view id, std::string_view key, std::string_view arguments) {
    auto frame = parse(arguments);
    if (key.size() != 64 || frame.size() != 4 || !game_uuid(frame.at("generation")) ||
        !valid_channel(frame.at("grant").get_ref<const std::string&>()) || !valid_channel(frame.at("question").get_ref<const std::string&>()) ||
        !frame.at("index").is_number_unsigned() || frame.at("index") == 0 || frame.at("index") > 64 ||
        (method != "mine.begin" && method != "mine.read" && method != "mine.stop")) throw std::invalid_argument("MC protocol: invalid mining call");
    frame.update(Json{{"version", 1}, {"id", id}, {"key", key}, {"method", method}});
    return frame.dump();
}

// 区分客户端调用开始、结束、协作停止和失败，不从客户端结束推断掉落。
// 步骤：匹配原编号 → 验证阶段与退出 → 对已开始动作核对目标坐标。
MineReply mine_reply(std::string_view wire, std::string_view id, std::string_view arguments) {
    const auto reply = parse(wire), args = parse(arguments);
    if (reply.size() != 3 || !reply.at("version").is_number_unsigned() || reply.at("version") != 1 ||
        reply.at("id").get_ref<const std::string&>() != id) throw std::invalid_argument("MC protocol: wrong mine reply");
    const auto& data = reply.at("action");
    if (data.size() != (data.contains("pickup") ? 10u : 9u)) throw std::invalid_argument("MC protocol: invalid mine shape");
    for (auto field : {"generation", "grant", "question", "index"})
        if (data.at(field) != args.at(field)) throw std::invalid_argument("MC protocol: wrong block order");
    const bool started = data.at("started").get<bool>(), exited = data.at("exited").get<bool>();
    const auto& phase = data.at("phase").get_ref<const std::string&>();
    const auto& target = data.at("target");
    if (!data.at("stopping").is_boolean() || !data.at("index").is_number_unsigned() ||
        (started && (target.size() != 3 || !target.at("x").is_number_integer() ||
            !target.at("y").is_number_integer() || !target.at("z").is_number_integer())) || (!started && !target.is_null()) ||
        (exited && phase != "clientFinished" && phase != "failed" && phase != "stopped") ||
        (!exited && phase != "preparing" && phase != "digging" && phase != "gathering") ||
        ((phase == "clientFinished" || phase == "digging" || phase == "gathering") && !started))
        throw std::invalid_argument("MC protocol: inconsistent mining outcome");
    if (data.contains("pickup") && !data.at("pickup").is_null()) {
        const auto& p = data.at("pickup");
        if (!started || p.size() != 6 || !game_uuid(p.at("stream")) || !p.at("first").is_number_unsigned() ||
            !p.at("last").is_number_unsigned() || p.at("first") == 0 || p.at("last") < p.at("first") ||
            !p.at("lower").is_number_unsigned() || !p.at("upper").is_number_unsigned() ||
            p.at("upper") > 1 || p.at("lower") > p.at("upper") || !p.at("onGround").is_boolean())
            throw std::invalid_argument("MC protocol: invalid pickup bounds");
    }
    return {exited, phase == "clientFinished", started, data.dump()};
}

// 地点和数量均取自原接单事实，控制帧不允许替换目的地。
std::string return_request(std::string_view method, std::string_view id, std::string_view key, std::string_view arguments) {
    auto frame = parse(arguments);
    if (key.size() != 64 || frame.size() != 3 || !game_uuid(frame.at("generation")) ||
        !valid_channel(frame.at("question").get_ref<const std::string&>()) || !valid_channel(frame.at("grant").get_ref<const std::string&>()) ||
        (method != "return.begin" && method != "return.read" && method != "return.stop")) throw std::invalid_argument("MC protocol: return");
    frame.update(Json{{"version", 1}, {"id", id}, {"key", key}, {"method", method}});
    return frame.dump();
}

// 只将抵达时的联合读数作为带回数量证据。
// 步骤：核对委托身份与阶段 → 检查到达位置 → 校验数量区间及观察来源。
InspectReply return_reply(std::string_view wire, std::string_view id, std::string_view arguments) {
    const auto reply = parse(wire), args = parse(arguments);
    if (reply.size() != 3 || !reply.at("version").is_number_unsigned() || reply.at("version") != 1 ||
        reply.at("id").get_ref<const std::string&>() != id) throw std::invalid_argument("MC protocol: wrong return reply");
    const auto& data = reply.at("action");
    if (data.size() != 9 || !valid_channel(data.at("place").get_ref<const std::string&>()))
        throw std::invalid_argument("MC protocol: return shape");
    for (auto key : {"generation", "question", "grant"}) if (data.at(key) != args.at(key)) throw std::invalid_argument("MC protocol: return identity");
    const bool exited = data.at("exited").get<bool>(), arrived = data.at("phase") == "arrived";
    if ((exited && !arrived && data.at("phase") != "failed" && data.at("phase") != "stopped") ||
        (!exited && data.at("phase") != "returning" && data.at("phase") != "stopping")) throw std::invalid_argument("MC protocol: return phase");
    if (arrived) {
        if (!data.at("stock").is_number_unsigned() || data.at("position").size() != 3) throw std::invalid_argument("MC protocol: arrival stock");
        for (auto axis : {"x", "y", "z"}) if (!data.at("position").at(axis).is_number()) throw std::invalid_argument("MC protocol: arrival position");
        const auto& proof = data.at("carried");
        if (!proof.is_null() && (proof.size() != 5 || !game_uuid(proof.at("stream")) || !proof.at("sample").is_number_unsigned() ||
            proof.at("sample") == 0 || proof.at("stock") != data.at("stock") || !proof.at("lower").is_number_unsigned() ||
            !proof.at("upper").is_number_unsigned() || proof.at("lower") > proof.at("upper") || proof.at("upper") > data.at("stock")))
            throw std::invalid_argument("MC protocol: carried range");
    }
    return {exited, arrived, false, data.dump()};
}

// 匹配具体发送回执，记录的是游戏角色已发出，不推断玩家已阅读。
std::string question_reply(std::string_view wire, std::string_view id, std::string_view arguments) {
    const auto reply = parse(wire), args = parse(arguments);
    if (reply.size() != 3 || !reply.at("version").is_number_unsigned() || reply.at("version") != 1 ||
        reply.at("id").get_ref<const std::string&>() != id) throw std::invalid_argument("MC protocol: wrong question reply");
    const auto& value = reply.at("question");
    if ((value.size() != 7 && value.size() != 9) || value.at("id") != args.at("question") || value.at("generation") != args.at("generation") ||
        !value.at("order").is_number_unsigned() || value.at("order") != args.at("order") || !game_uuid(value.at("owner")) ||
        !value.at("after").is_number_unsigned() || value.at("after") < args.at("order") || value.at("after") > 9007199254740991ULL ||
        value.at("sent") != true || value.at("text").get_ref<const std::string&>().empty() ||
        value.at("text").get_ref<const std::string&>().size() > 1024) throw std::invalid_argument("MC protocol: unbound question");
    if (value.size() == 9 && (value.at("material") != "minecraft:cobblestone" ||
        (!value.at("returnPlace").is_null() && !valid_channel(value.at("returnPlace").get_ref<const std::string&>()))))
        throw std::invalid_argument("MC protocol: invalid collection proposal");
    return value.dump();
}
}
