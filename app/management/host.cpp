#include "host.hpp"
#include <dafeiyu/digest.hpp>
#include <algorithm>
#include <cstdio>
#include <utility>

namespace dfy::management {
namespace {
constexpr auto interval = std::chrono::milliseconds(5);
constexpr std::size_t command_limit = 16;

// 依受信页面提供的回合或消息身份，在业务线程执行一次对话操作。
// 步骤：发送或排队交给应用 → 按原待发消息恢复或移除 → 对原回合提交停止或清空。
DialogueReceipt apply_dialogue(app::Application& application, const DialogueCommand& command) {
    using State = DialogueCommandState;
    if (command.action == DialogueAction::send) {
        const auto result = application.submit_dialogue(command.text);
        return {command.id,State::accepted,{result.session,result.turn}};
    }

    auto& conversation = application.conversation();
    if (command.action == DialogueAction::append) {
        const auto receipt = application.append_dialogue(command.target,command.text);
        State state = State::rejected;
        switch (receipt.state) {
        case agent::InputAdmission::accepted: state = State::accepted; break;
        case agent::InputAdmission::closed: state = State::stale; break;
        case agent::InputAdmission::full: state = State::full; break;
        case agent::InputAdmission::unavailable: state = State::unavailable; break;
        case agent::InputAdmission::rejected: break;
        }
        return {command.id,state,command.target,receipt.message};
    }
    if (command.action == DialogueAction::enqueue) {
        const auto receipt = application.enqueue_dialogue(command.target,command.text);
        State state = State::rejected;
        switch (receipt.state) {
        case app::QueueAdmission::accepted: state = State::accepted; break;
        case app::QueueAdmission::full: state = State::full; break;
        case app::QueueAdmission::stale: state = State::stale; break;
        case app::QueueAdmission::rejected: break;
        }
        return {command.id,state,{command.target.session,0},receipt.message};
    }
    if (command.action == DialogueAction::remove || command.action == DialogueAction::resume) {
        const bool matched = command.target.session == conversation.id() && (command.action == DialogueAction::remove
            ? application.remove_queued_dialogue(command.message) : application.resume_dialogue_queue(command.message));
        return {command.id,matched ? State::accepted : State::stale,command.target,command.message};
    }
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
Host::Host(Open open, Drive drive)
    : worker_([this,open = std::move(open),drive = std::move(drive)](std::stop_token stop) { run(stop,open,drive); }) {}

// 返回脱离业务对象寿命的当前值。
std::optional<app::RunStatusSnapshot> Host::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

// 读取最近的本体位置，查询不驱动原生房间。
std::optional<RoomLocation> Host::room_location() const {
    std::lock_guard lock(mutex_);
    return room_location_;
}

// 返回只读快照，旧命令保留期外明确过期。
// 步骤：复制已发布正文和接纳标志 → 查找原命令回执或过期状态。
DialogueView Host::dialogue(std::uint64_t command) const {
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
DialogueReceipt Host::submit_dialogue(DialogueCommand command) {
    using State = DialogueCommandState;
    const bool with_text = command.action == DialogueAction::send || command.action == DialogueAction::enqueue || command.action == DialogueAction::append;
    const bool by_message = command.action == DialogueAction::resume || command.action == DialogueAction::remove;
    const bool target_valid = command.action == DialogueAction::send ? command.target == agent::RunId{} :
        command.target.session && (by_message ? !command.target.sequence : command.target.sequence != 0);
    if (!command.id || !target_valid || by_message != (command.message != 0) ||
        (with_text ? command.text.size() > app::dialogue_text_limit || command.text.find_first_not_of(" \t\r\n") == std::string::npos : !command.text.empty()))
        return {command.id,State::rejected,{}};
    auto fingerprint = with_text ? platform::sha256(command.text) : std::string{};

    std::lock_guard lock(mutex_);
    if (command.id <= latest_command_) {
        const auto record = std::ranges::find_if(commands_,[&](const auto& item) { return item.receipt.command == command.id; });
        if (record == commands_.end()) return {command.id,State::expired,{}};
        if (record->action != command.action || record->target != command.target || record->message != command.message ||
            record->fingerprint != fingerprint)
            return {command.id,State::conflict,{}};
        return record->receipt;
    }
    if (!display_ || exited_ || worker_.get_stop_token().stop_requested()) return {command.id,State::unavailable,{}};

    auto& slot = command.action == DialogueAction::cancel ? stop_command_ : pending_;
    const bool needs_idle = command.action == DialogueAction::send || command.action == DialogueAction::clear;
    const bool available = !slot && (!needs_idle || can_submit_);
    const DialogueReceipt receipt{command.id,available ? State::queued : State::busy,command.target};
    commands_.push_back({receipt,command.action,command.target,command.message,std::move(fingerprint)});
    latest_command_ = command.id;
    if (available) {
        slot.emplace(std::move(command));
        can_submit_ = false;
    }
    if (commands_.size() > command_limit) {
        // 暂未交付的命令必须能在同一身份写回确认。
        const auto oldest = std::ranges::find_if(commands_,[](const auto& item) { return item.receipt.state != State::queued; });
        commands_.erase(oldest);
    }
    return receipt;
}

// 由唯一推进 Application 的线程交付命令，回执写回前保留对应槽位。
// 步骤：取走参数并保留占位 → 执行业务命令 → 写回同号结果并归还槽位。
void Host::advance_dialogue(app::Application& application, std::optional<DialogueCommand>& slot) {
    std::optional<DialogueCommand> command;
    {
        std::lock_guard lock(mutex_);
        if (!slot) return;
        command.emplace(std::move(*slot));
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
    slot.reset();
}

// 只在业务线程拥有和使用应用服务。
// 步骤：创建应用 → 推进原工作并发布快照 → 有序关闭并报告退出。
void Host::run(std::stop_token stop, const Open& open, const Drive& drive) noexcept {
    try {
        const auto application = open();
        auto& conversation = application->conversation();
        const auto layout = application->room().layout();
        std::shared_ptr<const app::ConversationDisplay> display;
        while (!stop.stop_requested()) {
            if (drive) drive(*application);
            advance_dialogue(*application,stop_command_);
            advance_dialogue(*application,pending_);
            if (application->advance_plugins() == runtime::RunResult::failed) std::fputs("A plugin notification failed.\n",stderr);
            conversation.advance();

            const auto snapshot = application->run_status();
            const auto self = application->room().self(layout.room);
            std::optional<RoomLocation> location;
            if (self) {
                const auto place = std::ranges::find(layout.places,self->place,&app::KnownPlace::id);
                if (place != layout.places.end()) location = RoomLocation{*self,layout.version,layout.name,place->name};
            }
            const auto& visible = conversation.display();
            if (!display || display->revision != visible.revision) display = std::make_shared<const app::ConversationDisplay>(visible);
            {
                std::lock_guard lock(mutex_);
                snapshot_ = snapshot;
                room_location_ = location;
                display_ = display;
                can_submit_ = conversation.accepting() && !conversation.busy() && !pending_ && !stop_command_ &&
                    (visible.queue.empty() || visible.queue_paused);
            }
            std::this_thread::sleep_for(interval);
        }
        shutdown(*application);
    } catch (const std::exception& error) { failed_ = true; std::fprintf(stderr,"UI application host: %s\n",error.what()); }
    catch (...) { failed_ = true; }
    exited_ = true;
}
}
