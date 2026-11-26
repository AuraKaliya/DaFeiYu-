#pragma once
#include <dafeiyu/mailboxes.hpp>
#include <map>

namespace dfy::app {
// 一个来源可包含面向本体和面向插件的多个实际地址，代次随原实例固定。
struct MailboxSource {
    std::string label, plugin;
    runtime::DomainId generation;
    std::vector<events::MailboxBinding> bindings;
};
struct MailboxDirectory {
    std::uint64_t revision;
    std::int64_t sampled_utc_ms;
    std::map<std::string,MailboxSource> sources;
};
}
