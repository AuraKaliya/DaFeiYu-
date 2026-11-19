#include "companion.hpp"
#include <algorithm>

namespace dfy::app {
// 汇集一个查看任务的事实，后续问题、授权及执行必须逐段匹配原身份。
// 步骤：复制原查看与介入 → 核对原问题及授权 → 关联接单、采掘与到岸读数。
BoardTask CompanionSession::current_board_task() const {
    const auto& visit = visit_.value();
    BoardTask task{visit, {}, {}, {}, {}};
    if (address_ == visit.address) task.player_input_gap = inbox_.status().gap;
    if (review_ && review_->address == visit.address && review_->task == visit.task && review_->order == visit.order)
        task.review = review_;

    const auto& question = questions_.current();
    if (!question || question->address != visit.address || question->task != visit.task ||
        question->order != visit.order || question->owner != visit.owner) return task;
    task.question = question;
    const auto& grant = questions_.grant();
    if (!grant || grant->address != visit.address || grant->source_task != visit.task || grant->order != visit.order ||
        grant->question != question->id || grant->owner != visit.owner) return task;

    const auto& delivery = delivery_.status();
    // 授权与派发之间也可查询，此时原游戏尚无接单回执。
    task.collection.emplace(BoardCollection{*grant, {}, {}});
    if (delivery.grant == grant->id) task.collection->delivery = delivery;
    if (mining_.status().grant == grant->id) task.collection->mining = mining_.status();
    return task;
}

// 在宿主线程读取完整业务快照，消息按原会话游标增量返回。
// 步骤：列出有界保留任务 → 定位当前或历史查看 → 筛选同一来源和任务的消息。
CompanionBoard CompanionSession::board(std::optional<std::uint64_t> task, std::uint64_t after) const {
    CompanionBoard result{id(), {}, discarded_through_, false, false, {}, messages_.since(after)};
    for (std::size_t n = 0; n < past_visits_.size(); ++n) {
        const auto& old = past_visits_[(next_past_ + n) % past_visits_.size()];
        if (!old) continue;
        result.tasks.push_back(old->visit.task);
        if (task == old->visit.task) {
            result.selected = old;
            result.historical = true;
        }
    }
    if (visit_) {
        result.tasks.push_back(visit_->task);
        if (!task || *task == visit_->task) result.selected = current_board_task();
    }
    result.binding_current = result.selected && address_ == result.selected->visit.address && bound();
    std::erase_if(result.messages.messages, [&](const auto& message) {
        return !result.selected || message.task != result.selected->visit.task || message.address != result.selected->visit.address;
    });
    return result;
}
}
