#pragma once
#include <functional>
#include <string_view>
#include <vector>

namespace dfy::app {
// 普通私聊的语气候选选项，查询向量和预编码范围向量使用同一向量空间。
struct ToneRecallOptions {
    bool include_casual = true;
    // 返回空向量表示当前无可用编码。
    std::function<std::vector<float>(std::string_view)> encode_query;
    std::vector<float> casual_scope_vector;
    double minimum_similarity = 1.0;
};
}
