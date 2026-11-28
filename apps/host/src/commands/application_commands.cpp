#include "application_commands.hpp"
#include "application.hpp"
#include "main_messages.hpp"
#include "diagnostic_files.hpp"
#include "diagnostics.hpp"
#include "plugin_commands.hpp"
#include <array>
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
    const bool scoped = words.size()==5 && words[3]=="--workspace" && !words[4].empty();
    if (words.size()!=3 && !scoped) throw std::invalid_argument(std::string("Usage: ")+(confirm ? "/run-confirmed" : "/run")+
        " PACKAGE/TOOL \"GOAL\" [--workspace \"DIRECTORY\"]");
    const auto directory = scoped ? std::filesystem::path(std::u8string(words[4].begin(),words[4].end())) : std::filesystem::path{};
    const auto status = confirm ? application.submit_confirmed(words[1],words[2],directory) : application.submit_read_only(words[1],words[2],directory);
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

// 展示一个原任务的来源链，缺失回执与已证实的效果分别标明。
// 步骤：核对保留窗口 → 展示原话、观察与答复 → 读取原游戏的接单和到场证据。
void write_board(const CompanionBoard& board, std::optional<std::uint64_t> requested, std::ostream& output) {
    output << "计划板 · 会话 " << board.session << (board.partner ? " · 可查事件" : " · 可查委托");
    for (auto task : board.tasks) output << ' ' << task;
    output << '\n';
    if (board.partner) {
        if (board.messages.gap) output << "早期消息已超出保留窗口。\n";
        if (board.messages.messages.empty()) output << "没有匹配的游戏对话。\n";
        for (const auto& msg : board.messages.messages) {
            const bool system = msg.kind == CompanionMessageKind::failed || msg.kind == CompanionMessageKind::status;
            const auto label = msg.kind == CompanionMessageKind::event ? " 现场 " :
                system ? " 系统 " : msg.from == CompanionSide::primary ? " 玩家 " : " 大肥鱼 ";
            output << '#' << msg.revision << label << msg.text << '\n';
        }
        return;
    }
    if (!board.selected) {
        output << (requested && *requested <= board.discarded_through ? "该委托已超出保留窗口。\n" : "没有匹配的查看委托。\n");
        return;
    }
    const auto& task = *board.selected;
    const auto& visit = task.visit;
    constexpr std::array visits{"待执行", "已提交", "到达", "已发现", "未核实", "已撤回"};
    output << (board.historical ? "历史查看 #" : "查看 #") << visit.task << " · 玩家消息 #" << visit.order << " · " << visit.text << '\n'
           << visits.at(static_cast<std::size_t>(visit.phase)) << " · " << visit.place << '\n'
           << "世界 " << visit.address.world << " · 绑定 " << visit.address.binding
           << (board.binding_current ? "（当前绑定）\n" : "（历史绑定）\n");
    if (!visit.evidence.empty()) output << "查看回执 " << visit.evidence << '\n';
    if (task.player_input_gap.value_or(false)) output << "玩家原话的读取窗口有缺口。\n";
    if (board.messages.gap) output << "主子消息有缺口，最早保留修订 #" << board.messages.oldest << "。\n";
    for (const auto& msg : board.messages.messages)
        output << "消息 #" << msg.revision << " ← #" << msg.reply_to << "  " << msg.text << '\n';
    if (task.review) output << "介入依据 #" << task.review->report << "  " << task.review->basis << "  " << task.review->verification << '\n';
    if (task.question) {
        constexpr std::array phases{"发送中", "待答", "已拒绝", "未核实", "已撤回", "已授权"};
        const auto& q = *task.question;
        output << "问题 #" << q.id << " 修订 " << q.revision << " · " << phases.at(static_cast<std::size_t>(q.phase)) << "  " << q.text << '\n';
    }
    if (!task.collection) return;

    const auto& collection = *task.collection;
    const auto& g = collection.grant;
    output << "采集 #" << g.id << " ← 问题 #" << g.question << " 修订 " << g.question_revision
           << (g.entry == AnswerEntry::game ? " · 游戏答复 #" : " · 管理入口答复 #") << g.command << "  " << g.answer << '\n';
    constexpr std::array receipts{"等游戏接收", "核实接单中", "游戏已接单", "原绑定已失效", "接单未核实"};
    output << (collection.delivery ? receipts.at(static_cast<std::size_t>(collection.delivery->phase)) : "已授权，暂无投递回执")
           << " · " << g.quantity << " × " << g.material << " → " << g.return_place << '\n';
    if (!collection.mining) return;
    const auto& mining = *collection.mining;
    constexpr std::array mining_phases{"空闲", "已提交采掘", "客户端已完成", "采掘失败", "采掘已撤回"};
    output << mining_phases.at(static_cast<std::size_t>(mining.phase)) << " · 已核对自采范围 "
           << mining.lower << "～" << mining.upper << " · 块编号 " << mining.index << '\n';
    if (mining.home.phase == ReturnPhase::requested) output << "已请求返程，等待到达回执。\n";
    if (mining.home.phase == ReturnPhase::failed || (mining.home.phase == ReturnPhase::uncertain && mining.home.evidence.empty()))
        output << "返程未核实。\n";
    if (!mining.home.evidence.empty()) {
        output << "到场总库存 " << mining.home.stock;
        if (mining.home.lower && mining.home.upper) output << " · 本次实带 " << *mining.home.lower << "～" << *mining.home.upper;
        else output << " · 实带来源未核实";
        output << "\n到达回执 " << mining.home.evidence << '\n';
    }
}

// 对显式加载的插件启用游戏收件，或按原会话查看、停止。
// 步骤：分派受信命令 → 校验实例及会话身份 → 输出接纳和实际状态。
void companion_command(Application& app, std::string_view command, std::ostream& output) {
    const auto words = command_words(command);
    if (words.size() == 3 && (words[1] == "open" || words[1] == "scenario")) {
        const auto original = app.companion_status();
        const auto id = app.open_companion(words[2]);
        try { app.listen_companion(id, words[1] == "open" ? CompanionMode::partner : CompanionMode::scenario); }
        catch (...) {
            if (!original || original->session != id) app.stop_companion(id);
            throw;
        }
        output << "Companion session " << id << " enabled.\n";
    } else if (words.size() == 3 && words[1] == "consult") {
        const auto run = app.submit_companion_exchange(words[2]);
        output << "Accepted run " << run.session << '/' << run.turn << ". View game receipts with /companion board.\n";
    } else if (words.size() == 3 && words[1] == "wake" && (words[2] == "auto" || words[2] == "manual")) {
        app.wake_for_plugin_mail(words[2] == "auto");
        output << "Plugin mailbox wake set to " << words[2] << ".\n";
    } else if (words.size() == 2 && words[1] == "review") {
        application_command(app,"/mail review",output,output);
    } else if ((words.size() == 3 || (words.size() == 4 && words[3] == "meet")) && words[1] == "where") {
        const auto answer = app.locate_companion(positive_id(words[2]), words.size() == 4 ? LocationPurpose::meeting : LocationPurpose::progress);
        switch (answer.basis) {
        case LocationBasis::recent: output << "Recent confirmed progress. "; break;
        case LocationBasis::confirm_meeting: output << "Meeting needs a new position. "; break;
        case LocationBasis::aging: output << "Last signal is aging. "; break;
        case LocationBasis::missing: output << "No confirmed position yet. "; break;
        case LocationBasis::disconnected: output << "Game connection unavailable. "; break;
        }
        if (answer.last) output << "Last confirmed: " << answer.last->payload << ". ";
        if (answer.refresh) output << "Refresh request " << answer.refresh << ". ";
        output << '\n';
    } else if (words.size() == 7 && words[1] == "approve") {
        const auto address = app.companion_address();
        if (!address || address->session != positive_id(words[2])) throw std::invalid_argument("companion: stale session");
        const auto grant = app.authorize_companion(*address, positive_id(words[3]), positive_id(words[4]),
            positive_id(words[5]), positive_id(words[6]));
        output << "Collection " << grant.id << " authorized for " << grant.quantity << " at " << grant.return_place << ".\n";
    } else if (words.size() == 3 && words[1] == "stop") {
        const auto id = positive_id(words[2]);
        app.stop_companion(id);
        output << "Stop requested for companion " << id << ".\n";
    } else if ((words.size() == 3 || words.size() == 4) && words[1] == "board") {
        const auto task = words.size() == 4 ? std::optional(positive_id(words[3])) : std::nullopt;
        const auto board = app.companion_board(positive_id(words[2]), task);
        if (!board) throw std::invalid_argument("companion: unknown board session");
        write_board(*board, task, output);
    } else if (words.size() == 2 && words[1] == "status") {
        const auto status = app.companion_status();
        if (!status) { output << "No companion session.\n"; return; }
        const auto input = app.companion_inbox(status->session);
        const auto phase = status->closed ? "closed" : status->stopping ? "stopping" : input && input->ready ? "listening" :
            input && input->readable ? "bridge readable" : "waiting for game";
        output << "Companion " << status->session << " (" << status->package << ") " << phase << ".\n";
        if (input && input->readable && !input->execution_state.empty())
            output << "游戏信箱可读 · 执行入口 " << input->execution_mode << ' ' << input->execution_state << '\n';
        if (status->control) {
            const auto& control = *status->control;
            if (control.order) output << "游戏玩家消息 #" << control.order;
            else output << "管理入口";
            output << " · " << (control.phase == ControlPhase::released ? "游戏已释放控制" :
                control.phase == ControlPhase::requested ? "已请求停止，等待游戏退出" : "游戏控制释放未核实") << '\n';
        }
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
        if (const auto review = app.companion_review(status->session)) {
            output << "探索记录 " << review->task << '/' << review->report << "  " << review->basis << '\n';
            if (review->inquiry) output << "主 Agent 追问 #" << review->inquiry << '\n';
            if (review->answer) output << "游戏答复 #" << review->answer << "  " << review->reply << '\n';
            if (!review->verification.empty()) output << review->verification << '\n';
        }
        if (const auto question = app.companion_question(status->session); question && question->phase == QuestionPhase::waiting)
            output << "Question " << question->id << " revision " << question->revision << ": " << question->text << '\n';
        if (const auto grant = app.companion_collection(status->session))
            output << "Collection " << grant->id << " authorized: " << grant->quantity << " " << grant->material << ".\n";
        if (const auto mined = app.companion_mining(status->session); mined && mined->grant) {
            output << "Self-collected range " << mined->lower << ".." << mined->upper << ", current block " << mined->index << ".\n";
            const auto& home = mined->home;
            if (home.phase == ReturnPhase::requested) output << "Return requested; awaiting arrival.\n";
            else if (home.phase == ReturnPhase::failed) output << "Return not verified.\n";
            else if (home.phase != ReturnPhase::idle) {
                if (home.evidence.empty()) output << "Arrival unconfirmed.\n";
                else {
                    output << "At meeting point with " << home.stock << " total. ";
                    if (home.lower && home.upper) output << "Self-collected carried " << *home.lower << ".." << *home.upper << ".\n";
                    else output << "Carried origin unverified.\n";
                }
            }
        }
    } else throw std::invalid_argument("Usage: /companion open PACKAGE | scenario PACKAGE | consult \"GOAL\" | wake auto|manual | review | status | board SESSION [TASK] | where SESSION [meet] | stop SESSION | approve SESSION QUESTION REVISION REQUEST COUNT");
}
}

// 处理明确的终端控制命令，保留原回执和插件分派。
// 步骤：匹配命令族 → 读取确认或提交决定／目标 → 按原应用状态返回。
bool application_command(Application& application, std::string_view command, std::ostream& output, std::ostream& errors) {
    const auto name = command.substr(0,command.find_first_of(" \t"));
    if (name == "/companion") companion_command(application, command, output);
    else if (name=="/comparison") {
        auto* memory=application.main_messages(); if (!memory) throw std::invalid_argument("comparison: profile required");
        memory->manage_case(command_words(command)); output<<"Comparison recorded.\n";
    }
    else if (name=="/knowledge") {
        auto* memory=application.main_messages();
        if (!memory) throw std::invalid_argument("knowledge: profile required");
        output<<memory->manage_knowledge(command_words(command,13)).dump()<<'\n';
    }
    else if (name == "/memory") {
        auto* memory = application.main_messages();
        if (!memory) throw std::invalid_argument("memory: persistent main chat required");
        const auto words = command_words(command);
        if (words.size()==2 && words[1]=="profile") output << memory->profile_info().dump() << '\n';
        else if (words.size()==2 && words[1]=="last") output << memory->last_memory_source() << '\n';
        else if (words.size()==4 && words[1]=="keep") {
            memory->change_memory(words[2],words[3]);
            output << "Remembered " << words[2] << '\n';
        } else if (words.size()==3 && words[1]=="revoke") {
            memory->change_memory(words[2],std::nullopt);
            output << "Revoked " << words[2] << '\n';
        } else if ((words.size()==7 || words.size()==8) && words[1]=="avatar") {
            int access=words[5]=="off" ? 0 : words[5]=="read" ? 1 : words[5]=="write" ? 2 : words[5]=="both" ? 3 : -1;
            if (access<0 || (words[6]!="private" && words[6]!="relay")) throw std::invalid_argument("memory: invalid avatar rights");
            if (words[6]=="relay") access|=4;
            memory->grant_avatar(words[2],words[3],words[4],access,words.size()==8 ? words[7] : "");
            output << "Updated avatar memory access.\n";
        } else throw std::invalid_argument("Usage: /memory profile | last | keep REF TOPIC | revoke REF | avatar PLUGIN CHANNEL ACCOUNT off|read|write|both private|relay [REF]");
    }
    else if (name == "/mail") {
        const auto words = command_words(command);
        if (words.size() == 3 && words[1] == "wake" && (words[2] == "auto" || words[2] == "manual")) {
            application.wake_for_plugin_mail(words[2] == "auto");
            output << "Plugin mailbox wake set to " << words[2] << ".\n";
            return true;
        }
        if (words.size() != 2 || words[1] != "review") throw std::invalid_argument("Usage: /mail review | /mail wake auto|manual");
        const auto result = application.review_mail();
        if (result == MailboxIntakeResult::rejected) throw std::runtime_error("Mailbox review failed.");
        output << (result == MailboxIntakeResult::idle ? "No mailbox item ready for review.\n" : "Mailbox item reviewed.\n");
    } else if (name == "/ask") {
        const auto words = command_words(command);
        if (words.size() != 4 || words[1] != "answer") throw std::invalid_argument("Usage: /ask answer ID \"ANSWER\"");
        if (!application.answer_console_ask(positive_id(words[2]),words[3])) throw std::invalid_argument("ASK reply not admitted.");
        output << "ASK reply recorded.\n";
    }
    else if (name == "/run" || name == "/run-confirmed") submit(application,command,name == "/run-confirmed",output,errors);
    else if (name == "/task") {
        const auto words = command_words(command,10);
        std::size_t next=5; std::optional<ComparisonTag> tag;
        if (words.size()>=8 && words[5]=="--case") {tag=ComparisonTag{words[6],words[7]}; next=8;}
        const bool numbered = words.size()==next+2 && words[next]=="--request";
        if (words.size()!=next && !numbered) throw std::invalid_argument("Usage: /task READ_TOOL CHANGE_TOOL EXPECTED GOAL [--case KEY CHECKPOINT] [--request ID]");
        const auto admission = numbered ? application.submit_verified_once(positive_id(words[next+1]),words[1],words[2],words[3],words[4],tag) :
            TaskAdmission{application.submit_verified(words[1],words[2],words[3],words[4],tag)};
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

// 在终端实际输出原问题，以成功刷新作为呈现回执。
void present_console_ask(Application& application, std::ostream& output) {
    const auto pending = application.console_ask();
    if (!pending) return;
    const auto& [id,spec] = *pending;
    output << spec.question << '\n';
    for (const auto& choice : spec.choices) output << "  " << choice << '\n';
    output << "/ask answer " << id << " \"ANSWER\"\n" << std::flush;
    if (output) application.answer_console_ask(id,{});
}
}
