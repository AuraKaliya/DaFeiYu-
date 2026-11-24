#pragma once
#include "partner_effect.hpp"
#include <array>
#include <cmath>
#include <stdexcept>

namespace dfy::mc {
// 检查配对玩家、物品注册名和主背包支持的普通交付数量。
inline bool valid_handoff_request(const nlohmann::json& args) {
    return args.contains("recipient") && game_uuid(args.at("recipient")) && args.contains("item") && args.at("item").is_string() &&
        !item_name(args.at("item").get_ref<const std::string&>()).empty() && args.contains("count") &&
        args.at("count").is_number_integer() && args.at("count") >= 1 && args.at("count") <= 2304;
}

// 对照同一批槽位核对跨堆取量及缺口；读数不全时只承认确实选中的普通物品。
// 步骤：核对库存覆盖 → 逐槽验证选择 → 对照总量、排除槽和未取得量。
inline void verify_handoff_stock(const nlohmann::json& stock, int count) {
    const auto& selected = stock.at("selected");
    const auto& status = stock.at("status");
    const auto number = [&](const char* key, int limit) {
        const auto& v = stock.at(key);
        if (!v.is_number_unsigned() || v > limit) throw std::invalid_argument("MC: invalid handoff inventory number");
        return v.get<int>();
    };
    const int available = number("available",16384), missing = number("missing",count);
    const int unknown = number("unknownSlots",128), excluded = number("excluded",128);
    if (stock.at("scope") != "main_and_hotbar" || !selected.is_array() || selected.size() > 128)
        throw std::invalid_argument("MC: invalid handoff storage coverage");
    if (status == "unavailable") {
        if (!selected.empty() || missing != count || available || unknown || excluded || stock.contains("start") || stock.contains("end"))
            throw std::invalid_argument("MC: unread storage reported a selection");
        return;
    }
    const int start = number("start",255), end = number("end",256), size = number("stackSize",128);
    if (end <= start || end-start > 128 || size < 1 || status != (unknown ? "partial" : "sampled") ||
        selected.size()+unknown+excluded > end-start || available > (end-start-unknown-excluded)*size)
        throw std::invalid_argument("MC: inconsistent storage coverage");
    int previous = start-1, held = 0, taken = 0;
    for (const auto& row : selected) {
        if (!row.at("slot").is_number_unsigned() || row.at("slot") < start || row.at("slot") >= end || row.at("slot") <= previous ||
            !row.at("held").is_number_unsigned() || row.at("held") < 1 || row.at("held") > size ||
            !row.at("take").is_number_unsigned() || row.at("take") < 1 || row.at("take") != std::min(count-taken,row.at("held").get<int>()))
            throw std::invalid_argument("MC: inconsistent handoff slot selection");
        previous = row.at("slot").get<int>();
        held += row.at("held").get<int>();
        taken += row.at("take").get<int>();
    }
    if (held > available || taken != std::min(available,count) || missing != count-taken)
        throw std::invalid_argument("MC: incorrect handoff inventory total");
}

// 绑定交物观察的原工作、当前接收者与位置覆盖，并按原数量核对库存分配。
// 步骤：检查请求和世界 → 核验可见实体及双方距离 → 复核窗口和备料。
inline void verify_handoff(const nlohmann::json& sample, const nlohmann::json& args, std::string_view id, const nlohmann::json& generation) {
    if (!valid_handoff_request(args) || sample.at("id").get_ref<const std::string&>() != id || sample.at("generation") != generation ||
        sample.at("recipient") != args.at("recipient") || sample.at("item").get_ref<const std::string&>() != item_name(args.at("item").get_ref<const std::string&>()) || sample.at("count") != args.at("count") ||
        !sample.at("sampledAtMs").is_number_unsigned() || !sample.at("dimension").is_string() || sample.at("dimension").get_ref<const std::string&>().empty())
        throw std::invalid_argument("MC: invalid handoff source");
    const auto& actor = sample.at("actor");
    const auto& target = sample.at("target");
    const auto& state = target.at("state");
    const bool tracked = state == "visible" || state == "unlocated";
    const auto point = [](const auto& p) {
        return p.is_object() && std::ranges::all_of(std::array{"x","y","z"},[&](const auto axis) {
            return p.contains(axis) && p.at(axis).is_number();
        });
    };
    if ((!actor.at("uuid").is_null() && !game_uuid(actor.at("uuid"))) || (!actor.at("position").is_null() && !point(actor.at("position"))) ||
        (!tracked && state != "not_tracked" && state != "not_listed") || target.contains("entityId") != tracked ||
        (tracked && !target.at("entityId").is_number_unsigned()) || target.contains("position") != (state == "visible") ||
        (state == "visible" && !point(target.at("position"))) || target.contains("distance") != (state == "visible" && !actor.at("position").is_null()))
        throw std::invalid_argument("MC: invalid handoff player coverage");
    if (target.contains("distance")) {
        const auto delta = [&](const char* axis) { return actor.at("position").at(axis).get<double>()-target.at("position").at(axis).get<double>(); };
        if (!target.at("distance").is_number() || std::abs(target.at("distance").get<double>()-std::hypot(delta("x"),delta("y"),delta("z"))) > 1e-8)
            throw std::invalid_argument("MC: inconsistent recipient distance");
    }
    if (sample.at("window") != "inventory" && sample.at("window") != "container" && sample.at("window") != "cursor_occupied")
        throw std::invalid_argument("MC: unknown handoff window");
    verify_handoff_stock(sample.at("stock"),args.at("count").get<int>());
}
}
