#pragma once
#include "management/resident.hpp"
#include <dafeiyu/line_input.hpp>
#include <iosfwd>

namespace dfy::management {
using TerminalTransport = std::function<nlohmann::json(const nlohmann::json&)>;
// 在同一原客户端及命令号上发送或查回终端命令，传输方负责一次管道交换。
nlohmann::json terminal_exchange(const TerminalTransport& send, const std::string& client, std::uint64_t id, nlohmann::json body);
// 用正式终端输入观察或操作原后台，普通离开只解除当前终端绑定。
int resident_chat(const std::filesystem::path& profile, const ResidentConnection& binding, platform::LineInput& input,
    std::ostream& output, std::ostream& errors, app::SoftwareLog* log);
}
