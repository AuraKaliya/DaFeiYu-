#include "decision_status.hpp"
#include <algorithm>
#include <stdexcept>

namespace dfy::agent::status_detail {
// 同一来源的事件在预览、已发布及重复请求间共用计次原件，只有实际发布才占保留窗口。
// 步骤：清理已丢弃的预留 → 核对原事件或来源淘汰水位 → 复用或建立有界预留。
std::vector<EventRef> EventAttention::prepare(const CountedWatches& watches) {
    if (watches.empty()) return {};
    std::erase_if(index_,[](const auto& row) { return row.second.expired(); });
    std::erase_if(retired_,[](const auto& row) { return !*row.second && row.second.use_count()==1; });
    std::vector<EventRef> result;
    result.reserve(watches.size());
    for (const auto& [id,watch] : watches) {
        const auto old = index_.find(id);
        if (auto event = old == index_.end() ? nullptr : old->second.lock()) {
            if (event->watch != watch) throw std::invalid_argument("status: conflicting attention event");
            result.push_back(std::move(event));
            continue;
        }
        auto source = retired_.find(id.first);
        if (source == retired_.end()) {
            if (retired_.size() >= 64) throw std::length_error("status: attention source limit");
            source = retired_.emplace(id.first,std::make_shared<std::uint64_t>(0)).first;
        }
        if (id.second <= *source->second) continue;
        if (index_.size() >= 4096) throw std::length_error("status: attention reservation limit");
        auto event = std::make_shared<EventWatch>(EventWatch{watch,id.second,watch.count,source->second});
        index_.insert_or_assign(id,event);
        result.push_back(std::move(event));
    }
    return result;
}

// 提交过的事件保留最近 128 个首次登记号，旧号即使其快照被淘汰也不能从头计次。
void EventAttention::publish(const std::vector<EventRef>& events) noexcept {
    for (const auto& event : events) {
        if (event->published) continue;
        auto& slot = recent_[next_];
        if (slot) *slot->retired = std::max(*slot->retired,slot->event);
        slot = event;
        event->published = true;
        next_ = (next_+1)%recent_.size();
    }
}
}
