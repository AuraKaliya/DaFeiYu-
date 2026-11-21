#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace dfy::mc {
// 按步骤要求分别核验拆除与实际取得的物品，保留原退出正文。
// 步骤：检查游戏终态 → 确认指定格确已清除 → 核对可选的入包物品。
inline bool verified_action(const nlohmann::json& result, std::string_view skill, const std::string& pickup) {
    if (result.value("status","") != "done") return false;
    if (skill != "collect") return true;
    if (const auto stock = result.find("stock"); stock != result.end())
        return stock->at("after").get<int>() >= stock->at("required").get<int>();
    const auto changes = result.find("changes");
    if (result.value("broken",0) != 1 || changes == result.end() || !changes->is_array() ||
        changes->empty() || changes->front().value("after","") != "air") return false;
    const auto inventory = result.find("inventoryDelta");
    return pickup.empty() || (inventory != result.end() && inventory->is_object() && inventory->value(pickup,0) > 0);
}
}
