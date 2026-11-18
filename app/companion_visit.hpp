#pragma once
#include "companion_messages.hpp"
#include <dafeiyu/session.hpp>

namespace dfy::app {
enum class VisitPhase { queued, submitted, arrived, observed, failed, withdrawn };
struct CompanionVisit {
    CompanionAddress address;
    std::uint64_t order = 0, task = 0;
    std::string owner, text, place;
    std::optional<agent::RunId> run;
    VisitPhase phase = VisitPhase::queued;
    std::string evidence;
};
// 从玩家委托生成本回合唯一允许的访问参数。
std::string visit_arguments(const CompanionVisit& visit);
// 只用原运行实际退出的工具证据判断到达和发现。
void verify_visit(CompanionVisit& visit, const agent::TurnRequest& turn, const std::string& tool);
}
