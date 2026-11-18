#include "companion_messages.hpp"
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <stdexcept>

namespace dfy::app {
namespace {
// 取得受信入口的序号槽，未知方向直接拒绝。
std::size_t index(CompanionSide side) {
    if (side == CompanionSide::primary) return 0;
    if (side == CompanionSide::game) return 1;
    throw std::invalid_argument("companion: unknown sender");
}
}
// 要求保留窗口至少容纳一条记录。
CompanionMessages::CompanionMessages(std::size_t limit) : limit_(limit) {
    if (!limit || limit > 256) throw std::invalid_argument("companion: invalid message capacity");
}
// 只在来源真正变化时重置当前委托与问题，旧消息仍可按修订读取。
void CompanionMessages::bind(std::optional<CompanionAddress> current) {
    if (current == current_) return;
    if (current && (!current->session || !current->instance || current->world.empty() || current->world.size() > 128 ||
        current->binding <= last_binding_))
        throw std::invalid_argument("companion: invalid binding");
    if (current) last_binding_ = current->binding;
    current_ = std::move(current);
    sent_.fill(0);
    task_ = inquiry_ = 0;
}
// 从指定参与者的上次接纳回执计算下一个可用命令号。
std::uint64_t CompanionMessages::next(CompanionSide side) const {
    const auto previous = sent_.at(index(side));
    if (previous == UINT64_MAX) throw std::overflow_error("companion: command exhausted");
    return previous + 1;
}
// 保持消息因果、方向及去重约束，在写入全部可分配数据后提交水位。
// 步骤：核对当前绑定与命令 → 复用原回执或核对父消息 → 记录并推进任务与询问状态。
CompanionMessage CompanionMessages::post(const CompanionAddress& address, CompanionSide from, CompanionMessageKind kind,
    std::uint64_t command, std::uint64_t reply_to, const std::string& text) {
    if (!current_ || address != *current_ || !command || text.empty() || text.size() > 4096 || !runtime::valid_utf8(text))
        throw std::invalid_argument("companion: invalid message or stale binding");
    const auto slot = index(from);
    if (command <= sent_[slot]) {
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            if (it->address == address && it->from == from && it->command == command) {
                if (it->kind == kind && it->reply_to == reply_to && it->text == text) return *it;
                throw std::invalid_argument("companion: conflicting receipt");
            }
        }
        throw std::invalid_argument("companion: old command outside receipt window");
    }
    if (command != next(from) || latest_ == UINT64_MAX) throw std::invalid_argument("companion: command sequence");
    const auto revision = latest_ + 1;
    auto task = task_, inquiry = inquiry_;
    switch (kind) {
        case CompanionMessageKind::delegation:
            if (from != CompanionSide::primary || reply_to || inquiry) throw std::invalid_argument("companion: invalid delegation");
            task = revision;
            break;
        case CompanionMessageKind::report:
            if (from != CompanionSide::game || !task || reply_to != task) throw std::invalid_argument("companion: invalid report");
            break;
        case CompanionMessageKind::inquiry: {
            const bool known = reply_to == task || std::any_of(history_.begin(), history_.end(), [&](const auto& entry) {
                return entry.address == address && entry.task == task && entry.revision == reply_to && entry.kind == CompanionMessageKind::report;
            });
            if (from != CompanionSide::primary || !task || !known || inquiry) throw std::invalid_argument("companion: invalid inquiry");
            inquiry = revision;
            break;
        }
        case CompanionMessageKind::answer:
            if (from != CompanionSide::game || !inquiry || reply_to != inquiry) throw std::invalid_argument("companion: stale inquiry");
            inquiry = 0;
            break;
        default: throw std::invalid_argument("companion: unknown message kind");
    }
    CompanionMessage accepted{address, from, kind, revision, command, task, reply_to, text};
    history_.push_back(accepted);
    if (history_.size() > limit_) history_.pop_front();
    latest_ = revision;
    sent_[slot] = command;
    task_ = task;
    inquiry_ = inquiry;
    return accepted;
}

// 复制有界历史，记录缺口不改变底层任务。
CompanionDelta CompanionMessages::since(std::uint64_t after) const {
    if (after > latest_) throw std::invalid_argument("companion: future cursor");
    const auto oldest = history_.empty() ? 1 : history_.front().revision;
    CompanionDelta result{oldest, latest_, after < oldest - 1, {}};
    for (const auto& message : history_) if (message.revision > after) result.messages.push_back(message);
    return result;
}
}
