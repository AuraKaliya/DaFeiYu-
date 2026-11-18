#pragma once
#include "companion_state.hpp"

namespace dfy::app {
enum class LocationPurpose { progress, meeting };
enum class LocationBasis { recent, confirm_meeting, aging, missing, disconnected };
struct LocationAnswer {
    LocationBasis basis = LocationBasis::missing;
    std::optional<CompanionSample> last;
    std::uint64_t refresh = 0;
};
// 根据玩家要了解进度还是当场会合、动作状态和两端观察年龄选择位置依据。
LocationBasis location_basis(const CompanionState& state, LocationPurpose purpose, bool moving,
    agent::ToolTime now, std::chrono::milliseconds refresh_interval);
}
