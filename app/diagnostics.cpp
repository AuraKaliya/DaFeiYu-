#include "diagnostics.hpp"
#include <ostream>

namespace dfy::app {
namespace {
// 把观察接纳状态映射为稳定名称，accepted 仅说明已进入通道。
std::string_view observation_name(events::ObservationStatus status) noexcept {
    switch (status) {
    case events::ObservationStatus::accepted: return "accepted (queued only)";
    case events::ObservationStatus::overloaded: return "capacity exhausted";
    case events::ObservationStatus::invalid: return "invalid notification";
    case events::ObservationStatus::failed: return "notification failure";
    }
    return "unknown";
}
}
// 通知失败只形成单独提示，成功接纳保持静默且不承诺观察者已经处理。
// 步骤：跳过已接纳回执 → 输出事件种类、回合身份和失败类别 → 明确问答结果独立。
void write_observation_warning(std::ostream& output, runtime::DomainId session, std::uint64_t turn,
    std::string_view kind, events::ObservationReceipt receipt) {
    if (receipt.status == events::ObservationStatus::accepted) return;
    output << "Warning: " << kind << " notification not accepted (" << observation_name(receipt.status)
        << "; session=" << session << ", turn=" << turn << "). Question result is unchanged.\n";
}
}
