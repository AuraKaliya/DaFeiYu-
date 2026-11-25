#include <dafeiyu/run_journal.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace dfy::agent {
static_assert(std::is_trivially_copyable_v<TraceFact>,"execution trace facts must remain allocation-free values");
// 构造时分配固定槽，后续执行期间写入只作值复制。
RunJournal::RunJournal(RunId run, std::size_t capacity) : run_(run) {
    validate_trace_id({run});
    if (!capacity || capacity > 1024) throw std::invalid_argument("trace: capacity must be 1..1024");
    slots_.resize(capacity);
}
// 投影错误不进入业务失败路径，已发布事实不会被调用方修订号覆盖。
// 步骤：校验来源与节点 → 签发单调修订 → 写入空槽或替换最旧保留项。
void RunJournal::append(TraceFact fact) noexcept {
    try {
        if (!valid_trace_id(fact.subject) || fact.subject.run != run_ || static_cast<unsigned>(fact.kind) > static_cast<unsigned>(TraceFactKind::exited) ||
            revision_ == std::numeric_limits<std::uint64_t>::max()) { failed_ = true; return; }
        fact.revision = ++revision_; fact.observed = std::chrono::steady_clock::now();
        if (size_ < slots_.size()) { slots_[(begin_+size_)%slots_.size()] = fact; ++size_; }
        else { slots_[begin_] = fact; begin_ = (begin_+1)%slots_.size(); }
    } catch (...) { failed_ = true; }
}
// 按来源修订复制保留窗口，查询不会删除、重排或生成事实。
// 步骤：校验游标和页长 → 标注保留窗口及故障 → 复制有限增量 → 标记剩余页。
TraceDelta RunJournal::read(std::uint64_t after, std::size_t limit) const {
    if (after > revision_ || !limit || limit > 1024) throw std::invalid_argument("trace: invalid cursor or page limit");
    const auto oldest = revision_-size_+1;
    TraceDelta result{run_,after,oldest,revision_,after < oldest-1,false,failed_,{}};
    result.facts.reserve(std::min(limit,size_));
    for (std::size_t index = 0; index < size_; ++index) {
        const auto& fact = slots_[(begin_+index)%slots_.size()]; if (fact.revision <= after) continue;
        if (result.facts.size() == limit) { result.more = true; break; }
        result.facts.push_back(fact);
    }
    return result;
}
}
