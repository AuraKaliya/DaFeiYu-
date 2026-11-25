#include "application.hpp"
#include "run_log.hpp"
#include "main_messages.hpp"
#include <dafeiyu/utf8.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>

namespace dfy::app {
// 只把用户原话排在所见回合之后，条数和全部待发正文分别受限。
// 步骤：复核会话与前驱 → 核对消息及剩余容量 → 分配身份并发布队列。
DialogueQueueReceipt Application::enqueue_dialogue(agent::RunId after, std::string text) {
    auto& display = conversation_.display_;
    const auto current = conversation_.status();
    if (!conversation_.accepting() || !current || after != agent::RunId{current->session,current->turn} ||
        display.turns.empty() || display.turns.back().run != after) return {QueueAdmission::stale};
    if (text.size() > dialogue_text_limit || text.find_first_not_of(" \t\r\n") == std::string::npos ||
        !runtime::valid_utf8(text) || queued_message_sequence_ == UINT64_MAX) return {QueueAdmission::rejected};
    std::size_t bytes = text.size();
    for (const auto& item : display.queue) bytes += item.text.size();
    if (display.queue.size() >= dialogue_queue_limit || bytes > dialogue_queue_byte_limit) return {QueueAdmission::full};

    const auto message = queued_message_sequence_+1;
    auto received = receive_dialogue(text,"dialogue.enqueue");
    display.queue.push_back({message,after.session,std::move(received.first),received.second});
    queued_message_sequence_ = message;
    if (display.queue.size() == 1) queued_after_ = after;
    ++display.revision;
    if (run_log_) run_log_->append("dialogue.queued",{{"message",message},{"session",after.session},{"after",after.sequence}});
    return {QueueAdmission::accepted,message};
}

// 上一实例的前驱结果需要重新核对，恢复待发投影时先暂停自动启动。
std::uint64_t Application::restore_queued_dialogue(const std::string& text) {
    auto& display = conversation_.display_;
    std::size_t bytes = text.size();
    for (const auto& entry : display.queue) bytes += entry.text.size();
    if (display.queue.size()>=dialogue_queue_limit || bytes>dialogue_queue_byte_limit || queued_message_sequence_==UINT64_MAX)
        throw std::length_error("dialogue: restored queue exceeds capacity");
    const auto native = queued_message_sequence_+1;
    display.queue.push_back({native,conversation_.id(),text});
    queued_message_sequence_ = native;
    display.queue_paused = true;
    ++display.revision;
    return native;
}

// 经用户确认恢复原队首，已经核对过的失败或取消无需再次暂停。
bool Application::resume_dialogue_queue(std::uint64_t first) {
    auto& display = conversation_.display_;
    if (!conversation_.accepting() || !display.queue_paused || display.queue.empty() || display.queue.front().message != first) return false;
    display.queue_paused = false;
    queued_after_.reset();
    ++display.revision;
    return true;
}

// 按原消息身份移除等待项，最后一项离开时一并收回暂停状态。
// 步骤：查找待发消息 → 擦除原项 → 更新队列状态及修订。
bool Application::remove_queued_dialogue(std::uint64_t message) {
    auto& display = conversation_.display_;
    const auto item = std::ranges::find(display.queue,message,&QueuedDialogue::message);
    if (item == display.queue.end()) return false;
    if (main_messages_) main_messages_->remove_queue(message);
    display.queue.erase(item);
    if (display.queue.empty()) {
        display.queue_paused = false;
        queued_after_.reset();
    }
    ++display.revision;
    return true;
}

// 由业务线程根据前一轮实际结果和退出逐条启动待发对话。
// 步骤：失败或取消时暂停 → 等待实际退出 → 经正式入口接纳 → 关联消息与新回合。
void Application::advance_dialogue_queue() {
    using runtime::OperationState;
    auto& display = conversation_.display_;
    if (display.queue.empty() || display.queue_paused || !conversation_.accepting()) return;
    if (queued_after_) {
        const auto previous = conversation_.status();
        if (!previous || *queued_after_ != agent::RunId{previous->session,previous->turn} ||
            previous->result == OperationState::failed || previous->result == OperationState::cancelled) {
            display.queue_paused = true;
            ++display.revision;
            return;
        }
        if (previous->result == OperationState::pending || previous->exit == OperationState::pending) return;
    }
    if (conversation_.busy()) return;

    const auto message = display.queue.front().message;
    try {
        if (main_messages_) main_messages_->start_queue(message);
        const auto started = begin_dialogue(display.queue.front().text,display.queue.front().mail);
        queued_after_ = agent::RunId{started.session,started.turn};
    } catch (const std::exception&) {
        // 接纳失败保留原文，只有用户显式恢复才会再次尝试。
        display.queue_paused = true;
        ++display.revision;
        return;
    }
    display.turns.back().queued_message = message;
    if (main_messages_) main_messages_->started_queue(message,*queued_after_);
    display.queue.erase(display.queue.begin());
    ++display.revision;
    if (run_log_) run_log_->append("dialogue.queue_started",{{"message",message},{"session",queued_after_->session},{"run",queued_after_->sequence}});
}
}
