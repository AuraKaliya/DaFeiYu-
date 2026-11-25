#pragma once
#include <dafeiyu/event_store.hpp>
#include <functional>
#include <string>
#include <vector>

namespace dfy::plugins {
struct RoutedMailbox {
    std::string category;
    std::uint32_t version = 0, flags = 0;
};
// 宿主取得的事件和解析结果，来源信息由原实例补齐。
struct RoutedEvent {
    events::EventId id{};
    runtime::DomainId owner = 0;
    std::string plugin, type, payload, projection;
    std::uint32_t version = 0, kind = 0, status = 0;
    RoutedMailbox mailbox;
    std::vector<RoutedMailbox> mailbox_categories;
};
// 只观察已接纳的原事件，回调期间借用记录。
using EventCapture = std::function<void(const RoutedEvent&)>;
}
