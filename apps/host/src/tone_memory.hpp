#pragma once
#include <dafeiyu/local_embedding.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace dfy::app {
inline constexpr std::string_view casual_tone_scope="主人私聊中随口分享日常小烦恼，希望自然接话";
class MessageStore;

// 普通私聊的语气候选选项，查询向量和预编码范围向量使用同一向量空间。
struct ToneRecallOptions {
    bool include_casual = true;
    // 返回空向量表示当前无可用编码。
    std::function<std::vector<float>(std::string_view)> encode_query;
    std::vector<float> casual_scope_vector;
    double minimum_similarity = 1.0;
    // 后台准备完成后借用本版本固定的范围向量。
    std::function<std::shared_ptr<const std::vector<float>>()> prepared_scope;
    // 管理查询只报告编码器可用状态，不读取聊天原文。
    std::function<std::string_view()> encoder_state;
};
// 绑定本档案的单线程编码与持久范围向量，首包等待预算到期后回退。
ToneRecallOptions prepare_tone_encoder(std::shared_ptr<MessageStore> store,std::function<model::LocalEncoder()> open,
    double minimum_similarity,std::chrono::milliseconds query_budget);
}
