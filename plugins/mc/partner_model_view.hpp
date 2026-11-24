#pragma once
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace dfy::mc {
// 从现场副本生成送模正文，原始采样继续用于执行、登记和回查。
std::string model_scene_text(nlohmann::json snapshot);
}
