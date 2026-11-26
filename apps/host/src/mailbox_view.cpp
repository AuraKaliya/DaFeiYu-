#include "application.hpp"
#include "mailbox_view.hpp"
#include <chrono>

namespace dfy::app {
namespace {
// 保存宿主采样时的 UTC 毫秒，重复查看同一版本沿用该时刻。
std::int64_t sampled_now() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}
// 从实际地址构造管理目录，同包重新加载取得独立来源身份。
// 步骤：索引已加载代次 → 核对每个地址的绑定方 → 合并同来源地址并固定采样。
MailboxDirectory Application::mailbox_directory() const {
    const auto& network = *mailbox_host_.network;
    MailboxDirectory result{network.directory_revision(),sampled_now()};
    std::map<runtime::DomainId,std::string> active;
    for (const auto& package : plugins_.loaded()) active.emplace(plugins_.instance(package.id)->generation(),package.id);

    for (const auto& binding : network.bindings(mailbox_host_.identity)) {
        const auto peer = binding.sender==mailbox_host_.identity ? binding.receiver : binding.sender;
        std::string id, label, plugin;
        if (binding.mailbox==dialogue_mailbox_) { id = "builtin/dialogue"; label = "主聊天"; }
        else if (binding.mailbox==room_mailbox_) { id = "builtin/room"; label = "书房"; }
        else if (const auto owner=active.find(peer); owner!=active.end()) {
            plugin = owner->second;
            id = "plugin/"+plugin+"/"+std::to_string(peer);
            label = plugin;
        } else continue;
        auto row = result.sources.try_emplace(id,MailboxSource{label,plugin,peer,{}}).first;
        row->second.bindings.push_back(binding);
    }
    return result;
}

// 在同一业务线程为关联地址取样，未变来源保持自己的修订及采样时刻。
// 步骤：核对目录和网络水位 → 对照各来源的地址与原修订 → 发布同一时刻可核对的只读值。
std::shared_ptr<const MailboxView> sample_mailboxes(const Application& app, const std::shared_ptr<const MailboxView>& previous) {
    const auto& host = app.mailbox_host();
    const auto& network = *host.network;
    if (previous && previous->revision==network.revision()) return previous;
    auto next = std::make_shared<MailboxView>();
    next->revision = network.revision();
    next->sampled_utc_ms = sampled_now();
    next->directory = previous && previous->directory->revision==network.directory_revision()
        ? previous->directory : std::make_shared<const MailboxDirectory>(app.mailbox_directory());

    for (const auto& [id,source] : next->directory->sources) {
        std::shared_ptr<const MailboxSourceView> old;
        if (previous && previous->sources.contains(id)) old = previous->sources.at(id);
        bool same = old && old->addresses.size()==source.bindings.size();
        for (std::size_t i=0; same && i<source.bindings.size(); ++i)
            same = old->addresses[i].revision==network.revision(host.identity,source.bindings[i].mailbox);
        if (same) { next->sources.emplace(id,std::move(old)); continue; }

        auto current = std::make_shared<MailboxSourceView>();
        current->revision = next->revision;
        current->sampled_utc_ms = next->sampled_utc_ms;
        for (const auto& binding : source.bindings) current->addresses.push_back(network.inspect(host.identity,binding.mailbox));
        next->sources.emplace(id,std::move(current));
    }
    return next;
}
}
