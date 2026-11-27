#pragma once
#include <string>

namespace dfy::app {
// 采样对象与单位由不可变比较定义固定，检查点指明在比较哪次状态。
struct ComparisonTag { std::string name,checkpoint; };
}
