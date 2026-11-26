#pragma once
#include <dafeiyu/decision_status.hpp>
#include <filesystem>

namespace dfy::app {
// 对用户为本次委托指定的目录取一次宿主样本，保留路径、操作系统和可核实的时区。
agent::DecisionStatusSource workspace_environment(const std::filesystem::path& directory);
}
