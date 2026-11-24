#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace dfy::agent {
inline constexpr std::size_t turn_input_count_limit = 8;
inline constexpr std::size_t turn_input_byte_limit = 1024*1024;
enum class InputState { waiting, submitted, cancelled, deadline, limit, failed, closed };
enum class InputAdmission { accepted, closed, full, rejected, unavailable };
// 把收件状态映射为 Trace 与可信主面板共同使用的固定名称。
inline const char* input_state_name(InputState state) noexcept {
    switch (state) {
    case InputState::waiting: return "waiting";
    case InputState::submitted: return "submitted";
    case InputState::cancelled: return "cancelled";
    case InputState::deadline: return "deadline";
    case InputState::limit: return "limit";
    case InputState::failed: return "failed";
    case InputState::closed: return "closed";
    }
    return "unknown";
}
// 消息号只在所属回合内有效，实际提交后才记录模型步骤号。
struct TurnInput {
    std::uint64_t message;
    std::string text;
    InputState state = InputState::waiting;
    std::uint64_t model_step = 0;
};
struct InputReceipt {
    InputAdmission state;
    std::uint64_t message = 0;
};
}
