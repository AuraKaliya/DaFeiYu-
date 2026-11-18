#include "companion_location.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 使用宿主收件时间和插件报告的信号年龄，近期进展可直接回答，会合前重新读取。
// 步骤：区分未绑定与最后已知 → 判断会合需求 → 按移动状态核对两段时效。
LocationBasis location_basis(const CompanionState& state, LocationPurpose purpose, bool moving,
    agent::ToolTime now, std::chrono::milliseconds refresh_interval) {
    if (state.link == CompanionLink::unbound || state.link == CompanionLink::failed) return LocationBasis::disconnected;
    if (!state.last || state.link != CompanionLink::available) return LocationBasis::missing;
    if (purpose == LocationPurpose::meeting) return LocationBasis::confirm_meeting;
    if (now < state.last->received || refresh_interval.count() <= 0 ||
        refresh_interval > std::chrono::milliseconds::max() / 3) return LocationBasis::aging;
    // 取自同一插件的正常刷新周期，移动中的位置比静止状态更快失效。
    const auto budget = moving ? refresh_interval : refresh_interval * 3;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last->received);
    if (elapsed > budget) return LocationBasis::aging;
    const auto sample = nlohmann::json::parse(state.last->payload, nullptr, false);
    if (!sample.is_object() || !sample.contains("signalAgeMs") || !sample.at("signalAgeMs").is_number_unsigned())
        return LocationBasis::aging;
    return sample.at("signalAgeMs").get<std::uint64_t>() <= static_cast<std::uint64_t>((budget - elapsed).count()) ?
        LocationBasis::recent : LocationBasis::aging;
}
}
