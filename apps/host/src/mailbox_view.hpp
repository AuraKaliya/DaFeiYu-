#pragma once
#include <dafeiyu/mailboxes.hpp>
#include <map>
#include <memory>

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
struct MailboxSourceView {
    std::uint64_t revision;
    std::int64_t sampled_utc_ms;
    std::vector<events::MailboxInspection> addresses;
};
struct MailboxView {
    std::uint64_t revision;
    std::int64_t sampled_utc_ms;
    std::shared_ptr<const MailboxDirectory> directory;
    std::map<std::string,std::shared_ptr<const MailboxSourceView>> sources;
};
class Application;
// 只在宿主业务线程发布已变化的来源快照，重复读取可共享原版本。
std::shared_ptr<const MailboxView> sample_mailboxes(const Application& app, const std::shared_ptr<const MailboxView>& previous);
}
