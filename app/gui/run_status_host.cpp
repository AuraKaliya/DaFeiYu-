#include "run_status_host.hpp"
#include <dafeiyu/digest.hpp>
#include <algorithm>
#include <cstdio>
#include <utility>

namespace dfy::ui {
namespace {
constexpr auto interval = std::chrono::milliseconds(5);
constexpr std::size_t command_limit = 16;

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

// 在窗口线程签收普通对话命令，原话只在单一待交付槽位保存。
// 步骤：核对正文并计算指纹 → 裁定同号请求和宿主状态 → 记录回执 → 在可用时交给业务线程。
DialogueReceipt RunStatusHost::submit_dialogue(std::uint64_t command, std::string text) {
    using State = DialogueCommandState;
    if (!command || text.size() > app::dialogue_text_limit || text.find_first_not_of(" \t\r\n") == std::string::npos)
        return {command,State::rejected,{}};
    auto fingerprint = platform::sha256(text);
    std::lock_guard lock(mutex_);
    if (command <= latest_command_) {
        const auto record = std::ranges::find_if(commands_,[=](const auto& item) { return item.receipt.command == command; });
        if (record == commands_.end()) return {command,State::expired,{}};
        if (record->fingerprint != fingerprint) return {command,State::conflict,{}};
        return record->receipt;
    }
    if (!display_ || exited_ || worker_.get_stop_token().stop_requested()) return {command,State::unavailable,{}};
    const DialogueReceipt receipt{command,can_submit_ ? State::queued : State::busy,{}};
    commands_.push_back({receipt,std::move(fingerprint)});
    latest_command_ = command;
    if (can_submit_) {
        pending_.emplace(DialogueCommand{command,std::move(text)});
        can_submit_ = false;
    }
    if (commands_.size() > command_limit) {
        // 暂未交付的命令必须能在同一身份写回确认。
        const auto oldest = std::ranges::find_if(commands_,[](const auto& item) { return item.receipt.state != State::queued; });
        commands_.erase(oldest);
    }
    return receipt;
}

// 由唯一推进 Application 的线程交付命令并关联实际接纳回执。
// 步骤：取出待交付命令 → 通过应用普通对话入口接纳 → 按同一命令号更新回执。
void RunStatusHost::advance_dialogue(app::Application& application) {
    std::optional<DialogueCommand> command;
    { std::lock_guard lock(mutex_); command = std::exchange(pending_,{}); }
    if (!command) return;

    using State = DialogueCommandState;
    DialogueReceipt receipt{command->id,State::rejected,{}};
    try {
        const auto result = application.submit_dialogue(command->text);
        receipt = {command->id,State::accepted,{result.session,result.turn}};
    } catch (const std::exception&) {
        // 拒绝回执只交付类别，异常正文可能包括宿主内部数据。
    }
    std::lock_guard lock(mutex_);
    const auto record = std::ranges::find_if(commands_,[&](const auto& item) { return item.receipt.command == command->id; });
    record->receipt = receipt;
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
