#pragma once
#include "application.hpp"

namespace dfy::app {
// 在独立运行目录驱动游戏伙伴，模型和游戏会话由调用方的 Application 持有。
void run_companion_host(Application& host, const std::filesystem::path& package, const std::filesystem::path& root, CompanionMode mode);
}
