#pragma once
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>

namespace dfy::mc {
// 校验英文注册名匹配完整词项，避免把不同变种合并。
inline bool knowledge_matches(std::string_view query, std::string_view name) {
    const auto word = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; };
    auto at = query.find(name);
    if (static_cast<unsigned char>(name.front()) >= 0x80) return at != query.npos;
    for (; at != query.npos; at = query.find(name,at+1)) {
        const auto end = at+name.size();
        if ((!at || !word(query[at-1])) && (end == query.size() || !word(query[end]))) return true;
    }
    return false;
}

// 在当前原版基线内按名称和自然用语检索可核对的游戏常识。
// 步骤：校验查询 → 匹配已知词条 → 返回版本、来源及候选事实。
inline nlohmann::json lookup_knowledge(const nlohmann::json& arguments) {
    struct Entry {
        std::string_view key, facts;
        std::array<std::string_view,4> names;
    };
    constexpr std::array entries{
        Entry{"grass_block",R"({"defaultDrop":"dirt","silkTouchDrop":"grass_block"})",
            {"grass_block","grass block","草方块","草皮"}},
        Entry{"dirt",R"({"defaultDrop":"dirt","toolRequired":false})",{"dirt","土块","泥土","soil"}},
        Entry{"oak_door",R"({"defaultDrop":"oak_door","height":2,"dropPerDoor":1,"support":"below_lower_half"})",
            {"oak_door","oak door","橡木门","木门"}}
    };
    auto query = arguments.at("query").get<std::string>();
    if (query.empty() || query.size() > 192) throw std::invalid_argument("MC: unsupported knowledge query length");
    for (auto& c : query) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
    auto matches = nlohmann::json::array();
    for (const auto& entry : entries) {
        for (const auto name : entry.names) {
            if (!knowledge_matches(query,name)) continue;
            matches.push_back({{"key",entry.key},{"facts",nlohmann::json::parse(entry.facts)}});
            break;
        }
    }
    const auto status = matches.empty() ? "unknown" : "found";
    return {{"status",status},{"edition","Java"},{"version","1.21.4"},{"source","vanilla_rules"},
        {"entries",std::move(matches)}};
}
}
