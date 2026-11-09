#pragma once
#include "common.hpp"
#include <dafeiyu/run_view.hpp>
namespace dfy {
sdk::RunView make_run_view(const Json& state, const Json& call_budget = Json{});
}
