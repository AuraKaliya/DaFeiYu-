#pragma once
#include "resident.hpp"
#include <dafeiyu/line_input.hpp>
#include <iosfwd>

namespace dfy::management {
// 用正式终端输入观察或操作原后台，普通离开只解除当前终端绑定。
int resident_chat(const std::filesystem::path& profile, const ResidentConnection& binding, platform::LineInput& input,
    std::ostream& output, std::ostream& errors, app::SoftwareLog* log);
}
