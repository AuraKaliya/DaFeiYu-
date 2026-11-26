#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <nlohmann/json_fwd.hpp>

namespace dfy::management {
struct PluginCommand {
    std::string action, id;
    std::uint64_t number = 0, revision = 0, generation = 0;
    std::string submission;
    // 同一窗口命令号只有完整意图一致时才可复用原回执。
    bool operator==(const PluginCommand&) const = default;
};
struct PluginOperation {
    std::string identity;
    PluginCommand command;
    std::string_view state = "queued", effect = "unconfirmed";
    std::uint64_t generation = 0, expected = 0;
    std::optional<std::uint32_t> native_code;
    std::string_view native_result;
    std::string originating_client;
    // 业务线程尚未确认终态的记录始终保留。
    bool unfinished() const noexcept { return state=="queued" || state=="running" || state=="pending"; }
};
class Host;
// 接纳受当前后台和客户端约束的包管理操作，或按原操作号查询。
nlohmann::json plugin_action_request(Host& host, const nlohmann::json& request, const std::string& client);
}
