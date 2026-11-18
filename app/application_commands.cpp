#include "application_commands.hpp"
#include "application.hpp"
#include "diagnostics.hpp"
#include "plugin_commands.hpp"
#include <charconv>
#include <ostream>

namespace dfy::app {
namespace {
// 将请求及修订号解析为完整的非零十进制身份。
std::uint64_t positive_id(const std::string& text) {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(),text.data()+text.size(),value);
    if (result.ec != std::errc{} || result.ptr != text.data()+text.size() || !value)
        throw std::invalid_argument("confirmation: expected a positive request and revision");
    return value;
}

// 提交用户指定的只读或需确认工具，并显示实际接纳标识。
// 步骤：核对参数 → 选择本轮授权模式 → 提交目标并展示接纳回执。
void submit(Application& application, std::string_view command, bool confirm, std::ostream& output, std::ostream& errors) {
    const auto words = command_words(command);
    if (words.size() != 3) throw std::invalid_argument(confirm ? "Usage: /run-confirmed PACKAGE/TOOL \"GOAL\"" : "Usage: /run PACKAGE/TOOL \"GOAL\"");
    const auto status = confirm ? application.submit_confirmed(words[1],words[2]) : application.submit_read_only(words[1],words[2]);
    output << "Accepted run " << status.session << '/' << status.turn
           << (confirm ? ". Use /confirmations to inspect pending actions.\n" : ". Use /status to inspect it.\n");
    write_observation_warning(errors,status.session,status.turn,"turn_started",*status.notifications.started);
}

// 从受信任的本地入口展示待审目标与原参数。
// 步骤：读取当前确认快照 → 展示仍待决定的项目 → 报告空查询。
void write_pending(const Application& application, std::ostream& output) {
    bool any = false;
    for (const auto& value : application.confirmations()) if (value.state == agent::ConfirmationState::waiting) {
        output << "Confirmation " << value.id << " revision " << value.revision << '\n'
               << "  " << value.target.tool_id << ' ' << value.target.arguments << '\n';
        any = true;
    }
    if (!any) output << "No pending confirmations.\n";
}
}

// 处理明确的终端控制命令，保留原回执和插件分派。
// 步骤：匹配命令族 → 读取确认或提交决定／目标 → 按原应用状态返回。
bool application_command(Application& application, std::string_view command, std::ostream& output, std::ostream& errors) {
    const auto name = command.substr(0,command.find_first_of(" \t"));
    if (name == "/run" || name == "/run-confirmed") submit(application,command,name == "/run-confirmed",output,errors);
    else if (name == "/confirmations") {
        if (command_words(command).size() != 1) throw std::invalid_argument("Usage: /confirmations");
        write_pending(application,output);
    } else if (name == "/approve" || name == "/deny") {
        const auto words = command_words(command);
        if (words.size() != 3) throw std::invalid_argument("Usage: /approve or /deny REQUEST REVISION");
        const auto receipt = application.decide(positive_id(words[1]),positive_id(words[2]),name == "/approve");
        output << (receipt.state == agent::ConfirmationState::approved ? "Approval" : "Refusal")
               << " recorded for " << receipt.request << " at revision " << receipt.revision << ".\n";
    } else return plugin_command(application.plugins(),command,output,errors);
    return true;
}
}
