#pragma once
#include <dafeiyu/plugin_manager.hpp>

namespace dfy::app {
class RunLog;
struct TransitionNotice {
    std::uint32_t point;
    runtime::DomainId owner;
    events::EventId origin;
    std::string category;
    std::uint32_t status = 0;
    std::uint64_t ask = 0;
    events::EventId message{};
};

// 通知原有序登记表，观察异常只写诊断。
void notify_transition(plugins::PluginManager& plugins, RunLog* log, const TransitionNotice& notice) noexcept;
}
