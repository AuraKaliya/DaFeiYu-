#pragma once
#include <dafeiyu/event_store.hpp>
#include <string>

namespace dfy::plugins {
// 宿主取得的事件和解析结果，来源信息由原实例补齐。
struct RoutedEvent {
    events::EventId id{};
    runtime::DomainId owner = 0;
    std::string plugin, type, payload, projection;
    std::uint32_t version = 0, kind = 0, status = 0;
};
}
