#include "application.hpp"
#include "mailbox_view.hpp"
#include <chrono>

namespace dfy::app {
// 从实际地址构造管理目录，同包重新加载取得独立来源身份。
// 步骤：索引已加载代次 → 核对每个地址的绑定方 → 合并同来源地址并固定采样。
MailboxDirectory Application::mailbox_directory() const {
    using namespace std::chrono;
    const auto& network = *mailbox_host_.network;
    MailboxDirectory result{network.directory_revision(),duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()};
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
}
