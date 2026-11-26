#include "workspace_environment.hpp"
#include <dafeiyu/runtime_ids.hpp>
#include <dafeiyu/turn_input.hpp>
#include <nlohmann/json.hpp>
#include <windows.h>

namespace dfy::app {
// 在工具委托接纳前核对其项目目录并固定本机时间信息，同一回合的续行复用原样本。
// 步骤：核对目录及 UTF-8 路径 → 读取本机系统与时区 → 签发独立来源和原采样号。
agent::DecisionStatusSource workspace_environment(const std::filesystem::path& directory) {
    using Json = nlohmann::json;
    std::error_code error;
    const auto path = std::filesystem::canonical(directory,error);
    if (error || !std::filesystem::is_directory(path,error)) throw std::invalid_argument("workspace: existing directory required");
    const auto utf8 = path.u8string();
    Json value{{"workspaceDirectory",std::string(utf8.begin(),utf8.end())},{"operatingSystem","Windows"}};
    const auto sampled = agent::message_utc_now();
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    const auto mode = GetDynamicTimeZoneInformation(&zone);
    if (mode!=TIME_ZONE_ID_INVALID) {
        value["utcOffsetMinutes"] = -zone.Bias-(mode==TIME_ZONE_ID_DAYLIGHT ? zone.DaylightBias : mode==TIME_ZONE_ID_STANDARD ? zone.StandardBias : 0);
        if (zone.TimeZoneKeyName[0]) {
            const auto name = std::filesystem::path(zone.TimeZoneKeyName).u8string();
            value["windowsTimeZone"] = std::string(name.begin(),name.end());
        }
    }
    static const auto definitions = Json::array({{{"key","host.development_environment"},{"version",1},
        {"description","用户为本次工具委托选定并由宿主核实的项目目录，以及采样时的 Windows 系统和时区。"},
        {"defaultAttention",{{"mode","explicit"}}},{"valueSchema",{{"type","object"},{"additionalProperties",false},
            {"properties",{{"workspaceDirectory",{{"type","string"}}},{"operatingSystem",{{"type","string"}}},
                {"windowsTimeZone",{{"type","string"}}},{"utcOffsetMinutes",{{"type","integer"}}}}},
            {"required",{"workspaceDirectory","operatingSystem"}}}}}}).dump();
    const auto fact = runtime::next_domain();
    return {"host",definitions,Json::array({{{"key","host.development_environment"},{"version",1},{"subject","workspace"},
        {"fact",fact},{"sampledUtcMs",sampled},{"value",std::move(value)}}}).dump()};
}
}
