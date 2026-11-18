#include "application_commands.hpp"
#include "application.hpp"
#include "diagnostics.hpp"
#include "plugin_commands.hpp"
#include <ostream>

namespace dfy::app {
// 将命令参数交给共享应用接纳，并报告原运行身份。
// 步骤：识别本地命令 → 校验精确参数 → 提交只读范围并显示回执或交由插件命令处理。
bool application_command(Application& application, std::string_view command, std::ostream& output, std::ostream& errors) {
    if (command.substr(0,command.find_first_of(" \t")) != "/run")
        return plugin_command(application.plugins(),command,output,errors);

    const auto words = command_words(command);
    if (words.size() != 3) throw std::invalid_argument("Usage: /run PACKAGE/TOOL \"GOAL\"");
    const auto status = application.submit_read_only(words[1],words[2]);
    output << "Accepted run " << status.session << '/' << status.turn << ". Use /status to inspect it.\n";
    write_observation_warning(errors,status.session,status.turn,"turn_started",*status.notifications.started);
    return true;
}
}
