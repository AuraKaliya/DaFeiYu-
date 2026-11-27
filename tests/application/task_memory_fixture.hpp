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
// 核对真实后端请求并返回工作原始回执。
std::vector<Json> trace(const fs::path& path,const std::vector<model::Request>& requests);
// 从真实插件入口执行语料中的任务并取回原 Trace。
std::vector<Json> perform(const Json& row, const fs::path& data, const fs::path& package, const std::string& goal,
    std::size_t& request_count, bool review = false);
// 为跨进程接续准备任务档案，或在一个快照副本上执行指定对照。
void task_resume(const fs::path& corpus, const fs::path& root, const fs::path& panel, const std::string& phase);
// 回放跨任务理解的通用语料并核对后台请求与私聊投递。
void understanding_cases(const fs::path& corpus, const fs::path& data, const fs::path& panel, const fs::path& counter);
// 在多种档案规模下用普通聊天入口测量查回、歧义、证据预算和往返耗时。
void understanding_recall(const fs::path& corpus, const fs::path& data,bool comparisons=false,bool audit=false);
// 构造指定规模的旧版跨工具档案并经过正式升级及主人别名管理入口。
void populate_comparison_archive(const fs::path& data,unsigned groups,bool varied=false);
// 将早期独立分歧埋入单组长期档案，测量实际保留的候选数量。
std::size_t populate_long_archive(const fs::path& data,unsigned natives);
// 同组历史扩容后验证首屏、深页、撤权以及前台实收预算。
void long_case_recall(const fs::path& corpus,const fs::path& data);
// 由两个隔离的业务插件发回原件，核对主人私聊中的比较、来源及撤权。
void channel_observations(const fs::path& corpus,const fs::path& data,const fs::path& freight,const fs::path& museum);
}
