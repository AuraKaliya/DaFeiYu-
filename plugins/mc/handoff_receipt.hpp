#pragma once
#include "partner_effect.hpp"
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>

namespace dfy::mc {
// 检查配对玩家、物品注册名和主背包支持的普通交付数量。
inline bool valid_handoff_request(const nlohmann::json& args) {
    return args.contains("recipient") && game_uuid(args.at("recipient")) && args.contains("item") && args.at("item").is_string() &&
        !item_name(args.at("item").get_ref<const std::string&>()).empty() && args.contains("count") &&
        args.at("count").is_number_integer() && args.at("count") >= 1 && args.at("count") <= 2304;
}

// 实际交物还须引用原备料观察，两种动作入口使用同一组参数。
inline bool valid_handoff_action(const nlohmann::json& args) {
    return args.is_object() && args.size() == 4 && valid_handoff_request(args) && args.contains("observation") &&
        args.at("observation").is_string() && !args.at("observation").get_ref<const std::string&>().empty() &&
        args.at("observation").get_ref<const std::string&>().size() <= 128;
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

// 核对交物双方的位置覆盖与可见实体，并用同次坐标复算距离。
// 步骤：检查角色及目标状态 → 校验状态对应的字段 → 核实时空读数中的双方距离。
inline void verify_handoff_location(const nlohmann::json& actor, const nlohmann::json& target) {
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
}

// 绑定交物观察的原工作、当前接收者与位置覆盖，并按原数量核对库存分配。
// 步骤：检查请求和世界 → 核验可见实体及双方距离 → 复核窗口和备料。
inline void verify_handoff(const nlohmann::json& sample, const nlohmann::json& args, std::string_view id, const nlohmann::json& generation) {
    if (!valid_handoff_request(args) || sample.at("id").get_ref<const std::string&>() != id || sample.at("generation") != generation ||
        sample.at("recipient") != args.at("recipient") || sample.at("item").get_ref<const std::string&>() != item_name(args.at("item").get_ref<const std::string&>()) || sample.at("count") != args.at("count") ||
        !sample.at("sampledAtMs").is_number_unsigned() || !sample.at("dimension").is_string() || sample.at("dimension").get_ref<const std::string&>().empty())
        throw std::invalid_argument("MC: invalid handoff source");
    verify_handoff_location(sample.at("actor"),sample.at("target"));
    if (sample.at("window") != "inventory" && sample.at("window") != "container" && sample.at("window") != "cursor_occupied")
        throw std::invalid_argument("MC: unknown handoff window");
    verify_handoff_stock(sample.at("stock"),args.at("count").get<int>());
}

// 把停止投物时的收件人采样绑定到原观察和真实未提交量。
// 步骤：核对角色、时序及剩余数量 → 复核可见范围 → 对照受阻原因与实际位置。
inline void verify_recipient_check(const nlohmann::json& result, bool exited) {
    const auto& check = result.at("recipientCheck");
    const auto& initial = result.at("handoff");
    const auto& transfer = result.at("transfer");
    const auto& target = check.at("target");
    const auto& reason = check.at("reasonCode");
    if (check.at("observation") != initial.at("id") || check.at("generation") != initial.at("generation") ||
        check.at("dimension") != initial.at("dimension") || check.at("recipient") != initial.at("recipient") ||
        check.at("actor").at("uuid") != initial.at("actor").at("uuid") || !check.at("sampledAtMs").is_number_unsigned() ||
        check.at("sampledAtMs") < initial.at("sampledAtMs") || (transfer.contains("sampledAtMs") && check.at("sampledAtMs") > transfer.at("sampledAtMs")) ||
        !check.at("remaining").is_number_unsigned() || check.at("remaining") == 0 ||
        check.at("remaining") != initial.at("count").get<int>()-transfer.at("submitted").get<int>() ||
        (exited && (result.at("status") == "done" || result.at("reasonCode") != reason)))
        throw std::invalid_argument("MC: invalid stopped handoff context");
    verify_handoff_location(check.at("actor"),target);
    if (reason == "handoff_recipient_unavailable") return;
    if (target.at("state") != "visible" || !target.contains("distance") || target.at("entityId") != initial.at("target").at("entityId"))
        throw std::invalid_argument("MC: stopped handoff lacks a recipient position");
    if (reason == "handoff_recipient_distant" && target.at("distance") > 3) return;
    if (reason == "handoff_recipient_moved" && target.at("distance") <= 3) {
        const auto delta = [&](const char* axis) {
            return target.at("position").at(axis).get<double>()-initial.at("target").at("position").at(axis).get<double>();
        };
        if (std::hypot(delta("x"),delta("y"),delta("z")) > 0.75) return;
    }
    throw std::invalid_argument("MC: stopped handoff disagrees with recipient position");
}

// 核对投出、原玩家收取、他人收取及两类待证数量，并复核可回查的服务端流和实体。
// 步骤：对齐提交总量 → 按取证覆盖复核来源 → 对照有界实体列表及省略量。
inline void verify_handoff_proof(const nlohmann::json& proof, int submitted, const nlohmann::json& recipient) {
    const auto number = [&](const char* key) {
        const auto& v = proof.at(key);
        if (!v.is_number_unsigned() || v > submitted) throw std::invalid_argument("MC: invalid handoff proof number");
        return v.get<int>();
    };
    const int thrown = number("thrown"), picked = number("recipientPicked"), other = number("otherPicked");
    if (number("throwUnknown") != submitted-thrown || number("pickupUnknown") != thrown-picked-other)
        throw std::invalid_argument("MC: unbalanced handoff proof");
    if (proof.at("source") == "unavailable") {
        if (thrown || proof.contains("stream") || proof.contains("stacks") || proof.contains("collectors") ||
            (proof.at("coverage") != "unavailable" && proof.at("coverage") != "interrupted"))
            throw std::invalid_argument("MC: unsourced handoff claim");
        return;
    }
    const auto& first = proof.at("afterSeq");
    const auto& last = proof.at("throughSeq");
    const auto& stacks = proof.at("stacks");
    if (proof.at("source") != "server_events" || (proof.at("coverage") != "read" && proof.at("coverage") != "interrupted") ||
        !game_uuid(proof.at("stream")) || !first.is_number_unsigned() || !last.is_number_unsigned() || first > last ||
        !stacks.is_array() || stacks.size() > 12) throw std::invalid_argument("MC: invalid handoff evidence window");
    std::set<std::string> ids;
    int shown = 0;
    for (const auto& stack : stacks) {
        if (!game_uuid(stack.at("uuid")) || !ids.insert(stack.at("uuid").get<std::string>()).second ||
            !stack.at("count").is_number_unsigned() || stack.at("count") == 0 || stack.at("count") > thrown)
            throw std::invalid_argument("MC: invalid handoff stack");
        for (const auto field : {"dropSeq","spawnSeq"}) if (!stack.at(field).is_number_unsigned() || stack.at(field) <= first || stack.at(field) > last)
            throw std::invalid_argument("MC: handoff entity outside the original stream window");
        shown += stack.at("count").get<int>();
    }
    const int omitted = number("omittedStacks");
    if (shown+omitted > thrown || (!omitted && shown != thrown) || (omitted && stacks.size() != 12))
        throw std::invalid_argument("MC: inconsistent handoff stack count");
    if (proof.contains("collectors") || proof.contains("omittedOtherUnits")) {
        const auto& collectors = proof.at("collectors");
        if (!collectors.is_array() || collectors.size() > 9) throw std::invalid_argument("MC: invalid handoff collectors");
        ids.clear();
        int for_recipient = 0, for_others = number("omittedOtherUnits");
        for (const auto& entry : collectors) {
            if (!game_uuid(entry.at("uuid")) || !ids.insert(entry.at("uuid").get<std::string>()).second ||
                !entry.at("count").is_number_unsigned() || entry.at("count") == 0 || entry.at("count") > thrown)
                throw std::invalid_argument("MC: invalid handoff collector identity");
            (entry.at("uuid") == recipient ? for_recipient : for_others) += entry.at("count").get<int>();
        }
        if (for_recipient != picked || for_others != other || (number("omittedOtherUnits") && collectors.size() != 9))
            throw std::invalid_argument("MC: inconsistent handoff collector totals");
    }
}

// 将只读复查限制在原交物的实体来源上界，并保留工作实际退出的状态。
// 步骤：绑定原工作及消息 → 检查原数量和采样 → 核对新收件证据来自封存的投物。
inline void verify_handoff_recheck(const nlohmann::json& sample, const nlohmann::json& args, std::string_view id, const nlohmann::json& context) {
    if (sample.at("id").get_ref<const std::string&>() != id || sample.at("work") != args.at("work") || sample.at("generation") != context.at("generation") ||
        !sample.at("order").is_number_unsigned() || sample.at("order") == 0 || sample.at("order") > context.at("order") ||
        sample.at("observation").get_ref<const std::string&>().empty() || !game_uuid(sample.at("recipient")) ||
        sample.at("dimension").get_ref<const std::string&>().empty() || item_name(sample.at("item").get_ref<const std::string&>()).empty() ||
        !sample.at("requested").is_number_unsigned() || sample.at("requested") == 0 || sample.at("requested") > 2304 ||
        !sample.at("submitted").is_number_unsigned() || sample.at("submitted") == 0 || sample.at("submitted") > sample.at("requested") ||
        !sample.at("sampledAtMs").is_number_unsigned() || (sample.at("originalStatus") != "done" && sample.at("originalStatus") != "blocked" &&
            sample.at("originalStatus") != "interrupted")) throw std::invalid_argument("MC: invalid original handoff work");
    const auto& proof = sample.at("receipt");
    verify_handoff_proof(proof,sample.at("submitted").get<int>(),sample.at("recipient"));
    const auto& seal = sample.at("sourceThrough");
    if (!seal.is_null() && !seal.is_number_unsigned()) throw std::invalid_argument("MC: invalid handoff source limit");
    if (proof.at("source") == "server_events") {
        if (seal.is_null() || seal < proof.at("afterSeq") || seal > proof.at("throughSeq")) throw std::invalid_argument("MC: unsealed handoff sources");
        for (const auto& stack : proof.at("stacks")) if (stack.at("dropSeq") > seal) throw std::invalid_argument("MC: later transfer attributed to old work");
    }
}

// 对照交物原观察核对已经提交的丢物操作与退出库存，实际收件由独立来源确认。
// 步骤：核对原工作及请求 → 区分已提交与调用返回 → 按同一角色的读数校验退出差量。
inline bool verify_transfer(const nlohmann::json& result, const nlohmann::json& args, const nlohmann::json& generation, bool exited) {
    if (result.value("reasonCode","") == "handoff_already_started" &&
        (result.at("originalWork").get_ref<const std::string&>().empty() || result.contains("transfer")))
        throw std::invalid_argument("MC: missing original handoff work");
    if (result.contains("handoff")) verify_handoff(result.at("handoff"),args,args.at("observation").get_ref<const std::string&>(),generation);
    if (!result.contains("transfer")) {
        if (result.contains("receipt") || result.contains("recipientCheck") || (exited && result.at("status") == "done"))
            throw std::invalid_argument("MC: handoff completion without execution");
        return false;
    }
    const auto& transfer = result.at("transfer");
    const auto& sample = result.at("handoff");
    const auto& item = sample.at("item").get_ref<const std::string&>();
    if (transfer.contains("progressAtMs") && (!transfer.at("progressAtMs").is_number_unsigned() ||
        transfer.at("progressAtMs") < sample.at("sampledAtMs") ||
        (transfer.contains("sampledAtMs") && transfer.at("progressAtMs") < transfer.at("sampledAtMs"))))
        throw std::invalid_argument("MC: invalid handoff progress time");
    if (transfer.at("observation") != sample.at("id") || transfer.at("recipient") != sample.at("recipient") ||
        transfer.at("item") != item || transfer.at("requested") != sample.at("count")) throw std::invalid_argument("MC: handoff work changed");
    const auto& clicks = transfer.at("attemptedClicks");
    const auto& submitted = transfer.at("submitted");
    if (!submitted.is_number_unsigned() || submitted > sample.at("count") || !clicks.is_number_unsigned() || clicks > submitted ||
        (clicks == 0) != (submitted == 0) || !transfer.at("returnedClicks").is_number_unsigned() || transfer.at("returnedClicks") > clicks ||
        transfer.at("sourceClaimed") != (clicks != 0) || !transfer.at("stockBefore").is_number_unsigned() || transfer.at("stockBefore") > 16384)
        throw std::invalid_argument("MC: invalid handoff operations");
    if (clicks != 0 && (sample.at("window") != "inventory" || sample.at("target").at("state") != "visible" ||
        !sample.at("target").contains("distance") || sample.at("target").at("distance") > 3 || sample.at("stock").at("missing") != 0))
        throw std::invalid_argument("MC: handoff started without verified conditions");
    if (result.contains("recipientCheck")) verify_recipient_check(result,exited);
    if (result.contains("receipt")) verify_handoff_proof(result.at("receipt"),submitted.get<int>(),sample.at("recipient"));
    if (!exited) return false;
    if (!transfer.at("sampledAtMs").is_number_unsigned() || transfer.at("sampledAtMs") < sample.at("sampledAtMs") ||
        !transfer.at("stockCurrent").is_boolean() || transfer.contains("stockAfter") != transfer.at("stockCurrent").get<bool>() ||
        result.contains("inventoryDelta") != transfer.contains("stockAfter")) throw std::invalid_argument("MC: invalid handoff stock source");
    if (transfer.contains("stockAfter") && (!transfer.at("stockAfter").is_number_unsigned() || transfer.at("stockAfter") > 16384 ||
        result.at("inventoryDelta").value(item,0) != transfer.at("stockAfter").get<int>()-transfer.at("stockBefore").get<int>()))
        throw std::invalid_argument("MC: inconsistent handoff stock change");
    if (result.at("status") == "done" && (!result.contains("receipt") || result.at("receipt").at("recipientPicked") != sample.at("count")))
        throw std::invalid_argument("MC: handoff completion without recipient evidence");
    return result.contains("receipt") && result.at("receipt").at("thrown") != 0;
}

// 从已核验的原工作读数生成在途进展，区分操作提交、投出实证和实际收取。
// 步骤：按当前操作选择阶段 → 带回最近采样与提交量 → 有收件读数时附上来源和去向。
inline std::string handoff_progress(const nlohmann::json& result) {
    if (!result.contains("transfer") || !result.at("transfer").contains("progressAtMs")) return {};
    const auto& transfer = result.at("transfer");
    nlohmann::json step{{"phase",result.contains("receipt") ? "awaiting_receipt" :
        transfer.at("attemptedClicks") == 0 ? "preparing_handoff" : "submitting_handoff"},
        {"sampledAtMs",transfer.at("progressAtMs")},{"observation",transfer.at("observation")},{"recipient",transfer.at("recipient")},
        {"item",transfer.at("item")},{"requested",transfer.at("requested")},{"submitted",transfer.at("submitted")}};
    if (result.contains("receipt")) {
        const auto& proof = result.at("receipt");
        for (const auto* key : {"coverage","thrown","recipientPicked","otherPicked","throwUnknown","pickupUnknown"})
            step[key] = proof.at(key);
    }
    return step.dump();
}
}
