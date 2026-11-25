#pragma once
#include <dafeiyu/session.hpp>
#include <dafeiyu/diagnostic_snapshot.hpp>
#include <iosfwd>

namespace dfy::app {
// 只报告未接纳的观察通知，明确独立于问答结果，不输出正文或凭据。
void write_observation_warning(std::ostream& output, runtime::DomainId session, std::uint64_t turn,
    std::string_view kind, events::ObservationReceipt receipt);
// 展示回合状态和相对单调时间；已退出快照的耗时固定，通知只表示接纳结果。
void write_turn_status(std::ostream& output, const agent::TurnDiagnostics& snapshot);
// 展示已读诊断的历史、实际退出、工具及版本摘要。
void write_diagnostic_review(std::ostream& output, const agent::DiagnosticSnapshot& snapshot);
}
