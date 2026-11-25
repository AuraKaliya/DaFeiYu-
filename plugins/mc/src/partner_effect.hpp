#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>
#include <string_view>

namespace dfy::mc {
// 返回已识别的原版物品标识结构，配方可用性由游戏能力另行判断。
inline std::string_view item_name(std::string_view name) noexcept {
    if (name.starts_with("minecraft:")) name.remove_prefix(10);
    return !name.empty() && name.size() <= 80 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == name.npos ?
        name : std::string_view{};
}

// 验证由宿主和游戏来源使用的规范 UUID。
inline bool game_uuid(const nlohmann::json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}

// 核对两种拾取入口共有的观察身份、物品注册名和整堆数量。
inline bool valid_pickup_request(const nlohmann::json& args) {
    return args.is_object() && args.size() == 4 && args.contains("observation") && args.at("observation").is_string() &&
        !args.at("observation").get_ref<const std::string&>().empty() && args.at("observation").get_ref<const std::string&>().size() <= 128 &&
        args.contains("entity") && game_uuid(args.at("entity")) && args.contains("item") && args.at("item").is_string() &&
        !item_name(args.at("item").get_ref<const std::string&>()).empty() && args.contains("count") &&
        args.at("count").is_number_integer() && args.at("count") >= 1 && args.at("count") <= 0x7fffffff;
}

// 按步骤要求分别核验拆除与实际取得的物品，保留原退出正文。
// 步骤：检查游戏终态 → 确认指定格确已清除 → 核对可选的入包物品。
inline bool verified_action(const nlohmann::json& result, std::string_view skill, const std::string& pickup) {
    if (result.value("status","") != "done") return false;
    if (skill != "collect") return true;
    if (const auto stock = result.find("stock"); pickup.empty() && stock != result.end())
        return stock->at("after").get<int>() >= stock->at("required").get<int>();
    const auto changes = result.find("changes");
    if (result.value("broken",0) != 1 || changes == result.end() || !changes->is_array() || changes->empty()) return false;
    const auto progress = result.find("progress");
    if (progress != result.end() && progress->contains("targetCell")) {
        const auto& target = progress->at("targetCell");
        if (!std::ranges::any_of(*changes,[&](const auto& cell) {
            return cell.at("x") == target.at("x") && cell.at("y") == target.at("y") && cell.at("z") == target.at("z") &&
                cell.at("before") == progress->at("block") && cell.at("after") == "air";
        })) return false;
    } else if (changes->front().value("after","") != "air") return false;
    const auto inventory = result.find("inventoryDelta");
    if (pickup.empty()) return true;
    const auto evidence = result.find("pickups");
    return inventory != result.end() && inventory->is_object() && inventory->value(pickup,0) > 0 &&
        evidence != result.end() && evidence->at("selfCollected").value(pickup,0) > 0;
}
}
