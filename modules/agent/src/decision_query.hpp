#pragma once
#include <dafeiyu/decision_status.hpp>
#include <dafeiyu/tool_catalog.hpp>

namespace dfy::agent {
// 在本轮已授权范围内签发只读状态查询，保留注册直到模型及工具实际退出。
std::unique_ptr<ToolCatalog> grant_status_reader(std::optional<ToolSnapshot>& tools,
    std::shared_ptr<DecisionStatusStore> store, std::uint64_t session, std::uint64_t epoch);
}
