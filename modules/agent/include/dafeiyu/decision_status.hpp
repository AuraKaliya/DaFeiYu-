#pragma once
#include <string>

namespace dfy::agent {
// 宿主绑定事实提供方；两个 JSON 数组分别声明状态项和提供当步原采样。
struct DecisionStatusSource {
    std::string owner, definitions, samples;
};
}
