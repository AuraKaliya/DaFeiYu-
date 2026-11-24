#include "run_status_host.hpp"
#include <dafeiyu/digest.hpp>
#include <algorithm>
#include <cstdio>
#include <utility>

namespace dfy::ui {
namespace {
constexpr auto interval = std::chrono::milliseconds(5);
constexpr std::size_t command_limit = 16;

// 依受信页面看见的原回合执行发送、停止或清空。
// 步骤：普通发送交给应用 → 控制命令复核显示身份与当前状态 → 提交取消或清空空闲上下文。
DialogueReceipt apply_dialogue(app::Application& application, const DialogueCommand& command) {
    using State = DialogueCommandState;
    if (command.action == DialogueAction::send) {
        const auto result = application.submit_dialogue(command.text);
        return {command.id,State::accepted,{result.session,result.turn}};
    }

    auto& conversation = application.conversation();
    const auto& turns = conversation.display().turns;
    if (turns.empty() || turns.back().run != command.target) return {command.id,State::stale,command.target};
    if (command.action == DialogueAction::cancel) {
        const auto current = application.run_status(command.target);
        if (!current || !conversation.cancel()) return {command.id,State::stale,command.target};
    } else {
        if (conversation.busy()) return {command.id,State::busy,command.target};
        conversation.clear();
    }
    return {command.id,State::accepted,command.target};
}

// 先等待会话退出，再推进原插件停止及回收边界。
// 步骤：关闭会话接纳 → 等待实际退出 → 有界推进插件关闭并检查结果。
void shutdown(app::Application& application) {
    auto& conversation = application.conversation();
    conversation.close();
    while (!conversation.closed()) {
        application.advance_plugins();
        conversation.advance();
        std::this_thread::sleep_for(interval);
    }

    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    auto result = application.close_plugins();
    while (result != plugins::ShutdownResult::closed) {
        if (result == plugins::ShutdownResult::failed || std::chrono::steady_clock::now() >= deadline ||
            application.advance_plugins() == runtime::RunResult::failed)
            throw std::runtime_error("UI: plugin shutdown incomplete");
        std::this_thread::sleep_for(interval);
        result = application.close_plugins();
    }
}
}

// 将创建和推进入口移交新业务线程，线程的寿命短于快照存储。
RunStatusHost::RunStatusHost(Open open, Drive drive)
    : worker_([this,open = std::move(open),drive = std::move(drive)](std::stop_token stop) { run(stop,open,drive); }) {}

// 返回脱离业务对象寿命的当前值。
std::optional<app::RunStatusSnapshot> RunStatusHost::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

// 返回只读快照，旧命令保留期外明确过期。
// 步骤：复制已发布正文和接纳标志 → 查找原命令回执或过期状态。
DialogueView RunStatusHost::dialogue(std::uint64_t command) const {
    std::lock_guard lock(mutex_);
    DialogueView view{display_,latest_command_,can_submit_ && !exited_ && !worker_.get_stop_token().stop_requested(),{}};
    if (command && command <= latest_command_) {
        const auto record = std::ranges::find_if(commands_,[=](const auto& item) { return item.receipt.command == command; });
        view.receipt = record != commands_.end() ? record->receipt : DialogueReceipt{command,DialogueCommandState::expired,{}};
    }
    return view;
}

// 签收页面命令并保留原目标，只有业务线程可以改变会话。
// 步骤：校验参数 → 复用原身份或报告冲突 → 核对槽位及忙碌状态 → 登记待交付命令。
DialogueReceipt RunStatusHost::submit_dialogue(DialogueCommand command) {
    using State = DialogueCommandState;
    const bool sending = command.action == DialogueAction::send;
    if (!command.id || (sending && (command.target != agent::RunId{} || command.text.size() > app::dialogue_text_limit ||
        command.text.find_first_not_of(" \t\r\n") == std::string::npos)) ||
        (!sending && (!command.target.session || !command.target.sequence || !command.text.empty())))
        return {command.id,State::rejected,{}};
    auto fingerprint = sending ? platform::sha256(command.text) : std::string{};

    std::lock_guard lock(mutex_);
    if (command.id <= latest_command_) {
        const auto record = std::ranges::find_if(commands_,[&](const auto& item) { return item.receipt.command == command.id; });
        if (record == commands_.end()) return {command.id,State::expired,{}};
        if (record->action != command.action || record->target != command.target || record->fingerprint != fingerprint)
            return {command.id,State::conflict,{}};
        return record->receipt;
    }
    if (!display_ || exited_ || worker_.get_stop_token().stop_requested()) return {command.id,State::unavailable,{}};

    const bool available = !pending_ && (command.action == DialogueAction::cancel || can_submit_);
    const DialogueReceipt receipt{command.id,available ? State::queued : State::busy,command.target};
    commands_.push_back({receipt,command.action,command.target,std::move(fingerprint)});
    latest_command_ = command.id;
    if (available) {
        pending_.emplace(std::move(command));
        can_submit_ = false;
    }
    if (commands_.size() > command_limit) {
        // 暂未交付的命令必须能在同一身份写回确认。
        const auto oldest = std::ranges::find_if(commands_,[](const auto& item) { return item.receipt.state != State::queued; });
        commands_.erase(oldest);
    }
    return receipt;
}

// 由唯一推进 Application 的线程交付命令，回执写回前保留单一槽位。
// 步骤：取走参数并保留占位 → 执行业务命令 → 写回同号结果并归还槽位。
void RunStatusHost::advance_dialogue(app::Application& application) {
    std::optional<DialogueCommand> command;
    {
        std::lock_guard lock(mutex_);
        if (!pending_) return;
        command.emplace(std::move(*pending_));
    }

    DialogueReceipt receipt{command->id,DialogueCommandState::rejected,command->target};
    try {
        receipt = apply_dialogue(application,*command);
    } catch (const std::exception&) {
        // 拒绝回执只交付类别，异常正文可能包括宿主内部数据。
    }
    std::lock_guard lock(mutex_);
    const auto record = std::ranges::find_if(commands_,[&](const auto& item) { return item.receipt.command == command->id; });
    record->receipt = receipt;
    pending_.reset();
}

// 只在业务线程拥有和使用应用服务。
// 步骤：创建应用 → 推进原工作并发布快照 → 有序关闭并报告退出。
void RunStatusHost::run(std::stop_token stop, const Open& open, const Drive& drive) noexcept {
    try {
        const auto application = open();
        auto& conversation = application->conversation();
        std::shared_ptr<const app::ConversationDisplay> display;
        while (!stop.stop_requested()) {
            if (drive) drive(*application);
            advance_dialogue(*application);
            if (application->advance_plugins() == runtime::RunResult::failed) std::fputs("A plugin notification failed.\n",stderr);
            conversation.advance();

            const auto snapshot = application->run_status();
            const auto& visible = conversation.display();
            if (!display || display->revision != visible.revision) display = std::make_shared<const app::ConversationDisplay>(visible);
            {
                std::lock_guard lock(mutex_);
                snapshot_ = snapshot;
                display_ = display;
                can_submit_ = conversation.accepting() && !conversation.busy() && !pending_;
            }
            std::this_thread::sleep_for(interval);
        }
        shutdown(*application);
    } catch (const std::exception& error) { failed_ = true; std::fprintf(stderr,"UI application host: %s\n",error.what()); }
    catch (...) { failed_ = true; }
    exited_ = true;
}
}
