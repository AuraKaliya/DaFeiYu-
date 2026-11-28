#pragma once
#include <dafeiyu/decision_status.hpp>
#include <nlohmann/json.hpp>

namespace dfy::app {
// 将已按当前读者裁定的知识发布版本投影为独立的宿主状态。
agent::DecisionStatusSource knowledge_source(const std::string& subject,std::uint64_t fact,const nlohmann::json& selection);
// 将外部文档页作为独立的可核对数据装配到主人私聊。
agent::DecisionStatusSource knowledge_document_source(const std::string& subject,std::uint64_t fact,const nlohmann::json& page);
// 提供库的概要与来源，用库名约束后续正文检索。
agent::DecisionStatusSource knowledge_catalog_source(const std::string& subject,std::uint64_t fact,const nlohmann::json& selection);
}
