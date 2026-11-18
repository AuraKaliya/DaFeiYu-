#include "application_commands.hpp"
#include "application.hpp"
#include "diagnostic_files.hpp"
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
        throw std::invalid_argument("command: expected a positive decimal identity");
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
// 步骤：处理未提交或仍等待 → 输出实际证据判断。
void write_verification(const std::optional<VerificationReport>& report, std::ostream& output) {
    if (!report) { output << "No verified task in the current or latest run.\n"; return; }

    output << "Run " << report->run.session << '/' << report->run.sequence << " verification ";
    if (!report->match) { output << "pending.\n"; return; }
    switch (*report->match) {
    case agent::ObservationMatch::satisfied: output << "matched the actual state.\n"; break;
    case agent::ObservationMatch::not_satisfied: output << "found a state mismatch.\n"; break;
    case agent::ObservationMatch::unknown: output << "could not establish evidence.\n"; break;
    }
}

// 查询已接纳请求及仍然保留的运行状态。
// 步骤：定位原接纳 → 核对结果身份 → 输出实时或最近值，缺失时报告已超出保留范围。
void write_task_status(const Application& application, std::uint64_t request, std::ostream& output) {
    const auto run = application.task_run(request);
    if (!run) throw std::invalid_argument("task: request receipt unavailable");
    output << "Task request " << request << " refers to run " << run->session << '/' << run->sequence << ".\n";
    if (const auto snapshot = application.run_status(*run)) {
        write_turn_status(output,snapshot->status);
        write_verification(snapshot->verification,output);
    } else output << "Run details are no longer retained.\n";
}

// 对显式加载的插件启用游戏收件，或按原会话查看、停止。
// 步骤：分派受信命令 → 校验实例及会话身份 → 输出接纳和实际状态。
void companion_command(Application& app, std::string_view command, std::ostream& output) {
    const auto words = command_words(command);
    if (words.size() == 3 && words[1] == "open") {
        const auto original = app.companion_status();
        const auto id = app.open_companion(words[2]);
        try { app.listen_companion(id); }
        catch (...) {
            if (!original || original->session != id) app.stop_companion(id);
            throw;
        }
        output << "Companion session " << id << " enabled.\n";
    } else if (words.size() == 3 && words[1] == "stop") {
        const auto id = positive_id(words[2]);
        app.stop_companion(id);
        output << "Stop requested for companion " << id << ".\n";
    } else if (words.size() == 2 && words[1] == "status") {
        const auto status = app.companion_status();
        if (!status) { output << "No companion session.\n"; return; }
        const auto input = app.companion_inbox(status->session);
        output << "Companion " << status->session << " (" << status->package << ") "
               << (status->closed ? "closed" : status->stopping ? "stopping" : input && input->ready ? "listening" : "waiting for game") << ".\n";
        if (const auto visit = app.companion_visit(status->session)) {
            output << "Visit " << visit->task << " at " << visit->place << ": ";
            switch (visit->phase) {
            case VisitPhase::queued: output << "queued"; break;
            case VisitPhase::submitted: output << "submitted"; break;
            case VisitPhase::arrived: output << "arrived"; break;
            case VisitPhase::observed: output << "observed"; break;
            case VisitPhase::failed: output << "unverified"; break;
            case VisitPhase::withdrawn: output << "withdrawn"; break;
            }
            output << ".\n";
        }
        if (const auto question = app.companion_question(status->session); question && question->phase == QuestionPhase::waiting)
            output << "Question " << question->id << " revision " << question->revision << ": " << question->text << '\n';
    } else throw std::invalid_argument("Usage: /companion open PACKAGE | status | stop SESSION");
}
}

// 处理明确的终端控制命令，保留原回执和插件分派。
// 步骤：匹配命令族 → 读取确认或提交决定／目标 → 按原应用状态返回。
bool application_command(Application& application, std::string_view command, std::ostream& output, std::ostream& errors) {
    const auto name = command.substr(0,command.find_first_of(" \t"));
    if (name == "/companion") companion_command(application, command, output);
    else if (name == "/run" || name == "/run-confirmed") submit(application,command,name == "/run-confirmed",output,errors);
    else if (name == "/task") {
        const auto words = command_words(command);
        const bool numbered = words.size() == 7 && words[5] == "--request";
        if (words.size() != 5 && !numbered)
            throw std::invalid_argument("Usage: /task READ_TOOL CHANGE_TOOL EXPECTED \"GOAL\" [--request ID]");
        const auto admission = numbered ? application.submit_verified_once(positive_id(words[6]),words[1],words[2],words[3],words[4]) :
            TaskAdmission{application.submit_verified(words[1],words[2],words[3],words[4])};
        if (admission.reused) output << "Original run " << admission.status.session << '/' << admission.status.turn << " retained.\n";
        else write_accepted(admission.status,"Use /confirmations for pending actions and /verification for actual state evidence.",output,errors);
    } else if (name == "/task-status") {
        const auto words = command_words(command);
        if (words.size() != 2) throw std::invalid_argument("Usage: /task-status REQUEST");
        write_task_status(application,positive_id(words[1]),output);
    } else if (name == "/verification") {
        if (command_words(command).size() != 1) throw std::invalid_argument("Usage: /verification");
        write_verification(application.verification(),output);
    } else if (name == "/diagnostic") {
        const auto words = command_words(command);
        if (words.size() != 3 || words[1] != "export") throw std::invalid_argument("Usage: /diagnostic export \"FILE\"");
        const auto snapshot = application.conversation().diagnostic();
        if (!snapshot) throw std::invalid_argument("diagnostic: no current or recent run");
        const auto path = std::filesystem::path(std::u8string(words[2].begin(),words[2].end()));
        write_diagnostic_file(path,*snapshot);
        output << "Diagnostic exported for run " << snapshot->timeline.key.run.session << '/' << snapshot->timeline.key.run.sequence << ".\n";
    } else if (name == "/confirmations") {
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
