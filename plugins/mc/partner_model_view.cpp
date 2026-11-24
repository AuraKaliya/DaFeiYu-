#include "partner_model_view.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;

// 将连续读数舍入到两位小数，并消除舍入产生的负零。
double displayed_number(double value) {
    if (std::trunc(value) == value) return value == 0 ? 0 : value;
    const double result = std::round(value * 100.0) / 100.0;
    return result == 0 ? 0 : result;
}

// 精简位置读数，在舍入跨过格界时附上按原始读数确定的格坐标。
// 步骤：校验三个坐标轴 → 对照展示前后的格坐标 → 按需补充原始所在格。
void display_position(Json& owner, const std::string& name, Json& position) {
    for (const auto axis : {"x","y","z"})
        if (!position.contains(axis) || !position.at(axis).is_number()) return;

    bool crossed = false;
    for (const auto axis : {"x","y","z"}) {
        const auto& coordinate = position.at(axis);
        if (!coordinate.is_number_float()) continue;
        const double raw = coordinate.get<double>();
        crossed |= std::floor(raw) != std::floor(displayed_number(raw));
    }
    if (crossed) {
        Json cell = Json::object();
        for (const auto axis : {"x","y","z"}) {
            const auto& coordinate = position.at(axis);
            const double whole = std::floor(coordinate.get<double>());
            cell[axis] = coordinate.is_number_float() && whole >= -0x1p63 && whole < 0x1p63 ?
                Json(static_cast<std::int64_t>(whole)) : coordinate;
        }
        owner.emplace(name + "Cell",std::move(cell));
    }
    for (const auto axis : {"x","y","z"}) {
        auto& coordinate = position.at(axis);
        if (coordinate.is_number_float()) coordinate = displayed_number(coordinate.get<double>());
    }
}

// 只处理现场定义的量值，保留命令参数、方块格和采样身份。
// 步骤：遍历事件和采样数组 → 分别处理位置与连续标量 → 展开内层现场。
void display_scene(Json& node) {
    if (node.is_array()) {
        for (auto& item : node) display_scene(item);
        return;
    }
    if (!node.is_object()) return;
    for (auto& [name,value] : node.items()) {
        if (name == "arguments" || name == "args") continue;
        if ((name == "position" || name == "player" || name == "eye" || name == "approachPosition") && value.contains("x"))
            display_position(node,name,value);
        else if ((name == "distance" || name == "vertical" || name == "yaw" || name == "pitch" || name == "angle") &&
                 value.is_number_float())
            value = displayed_number(value.get<double>());
        else display_scene(value);
    }
}
}

// 在写出送模文本前精简副本，调用方持有的游戏事实保持全精度。
std::string model_scene_text(nlohmann::json snapshot) {
    display_scene(snapshot);
    return snapshot.dump();
}
}
