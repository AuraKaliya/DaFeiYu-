#pragma once
#include <cstdint>
#include <string>

namespace dfy::agent {
enum class InputState { waiting, submitted, cancelled, deadline, limit, failed, closed };
enum class InputAdmission { accepted, closed, full, rejected, unavailable };
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
