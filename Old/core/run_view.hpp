#pragma once
#include "common.hpp"
#include <dafeiyu/run_view.hpp>

namespace dfy {
// 将核心任务状态投影为插件可读取的公开决策视图。
sdk::RunView make_run_view(const Json& state, const Json& call_budget = Json{});
}
