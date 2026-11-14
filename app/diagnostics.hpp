#pragma once
#include <dafeiyu/session.hpp>
#include <iosfwd>

namespace dfy::app {
// 只报告未接纳的观察通知，明确独立于问答结果，不输出正文或凭据。
void write_observation_warning(std::ostream& output, runtime::DomainId session, std::uint64_t turn,
    std::string_view kind, events::ObservationReceipt receipt);
}
