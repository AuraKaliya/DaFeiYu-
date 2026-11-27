#pragma once
#include <dafeiyu/decision_status.hpp>
#include <nlohmann/json.hpp>

namespace dfy::app {
// 将已按当前读者裁定的知识发布版本投影为独立的宿主状态。
agent::DecisionStatusSource knowledge_source(const std::string& subject,std::uint64_t fact,const nlohmann::json& selection);
}
