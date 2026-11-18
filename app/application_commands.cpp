#include "application_commands.hpp"
#include "application.hpp"
#include "diagnostics.hpp"
#include "plugin_commands.hpp"
#include <charconv>
#include <ostream>

namespace dfy::app {
namespace {
// 展示原应用接纳标识并报告独立的通知回执。
void write_accepted(const agent::TurnDiagnostics& status, std::string_view next, std::ostream& output, std::ostream& errors) {
    output << "Accepted run " << status.session << '/' << status.turn << ". " << next << '\n';
    write_observation_warning(errors,status.session,status.turn,"turn_started",*status.notifications.started);
}

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
    write_accepted(status,confirm ? "Use /confirmations to inspect pending actions." : "Use /status to inspect it.",output,errors);
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

// 展示与当前运行关联的宿主读回判定。
// 步骤：取得原状态 → 处理未提交或仍等待 → 输出实际证据判断。
void write_verification(const Application& application, std::ostream& output) {
    const auto report = application.verification();
    if (!report) { output << "No verified task in the current or latest run.\n"; return; }

    output << "Run " << report->run.session << '/' << report->run.sequence << " verification ";
    if (!report->match) { output << "pending.\n"; return; }
    switch (*report->match) {
    case agent::ObservationMatch::satisfied: output << "matched the actual state.\n"; break;
    case agent::ObservationMatch::not_satisfied: output << "found a state mismatch.\n"; break;
    case agent::ObservationMatch::unknown: output << "could not establish evidence.\n"; break;
    }
}
}

// 处理明确的终端控制命令，保留原回执和插件分派。
// 步骤：匹配命令族 → 读取确认或提交决定／目标 → 按原应用状态返回。
bool application_command(Application& application, std::string_view command, std::ostream& output, std::ostream& errors) {
    const auto name = command.substr(0,command.find_first_of(" \t"));
    if (name == "/run" || name == "/run-confirmed") submit(application,command,name == "/run-confirmed",output,errors);
    else if (name == "/task") {
        const auto words = command_words(command);
        if (words.size() != 5) throw std::invalid_argument("Usage: /task READ_TOOL CHANGE_TOOL EXPECTED \"GOAL\"");
        const auto status = application.submit_verified(words[1],words[2],words[3],words[4]);
        write_accepted(status,"Use /confirmations for pending actions and /verification for actual state evidence.",output,errors);
    } else if (name == "/verification") {
        if (command_words(command).size() != 1) throw std::invalid_argument("Usage: /verification");
        write_verification(application,output);
    }
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
