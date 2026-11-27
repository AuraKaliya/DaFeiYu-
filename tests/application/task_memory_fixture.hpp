#pragma once
#include "application.hpp"
#include <nlohmann/json.hpp>

namespace dfy::tests::task_memory {
using Json = nlohmann::json;
namespace fs = std::filesystem;
// 按语料预期报告失败。
void require(bool ok, const std::string& reason);
// 推进业务和退出并按场景裁定审批。
void drain(app::Application& host, const Json& row = Json::object());
// 从真实插件入口执行语料中的任务并取回原 Trace。
std::vector<Json> perform(const Json& row, const fs::path& data, const fs::path& package, const std::string& goal,
    std::size_t& request_count);
// 回放跨任务理解的通用语料并核对后台请求与私聊投递。
void understanding_cases(const fs::path& corpus, const fs::path& data, const fs::path& panel, const fs::path& counter);
// 在多种档案规模下用普通聊天入口测量查回、歧义、证据预算和往返耗时。
void understanding_recall(const fs::path& corpus, const fs::path& data);
}
