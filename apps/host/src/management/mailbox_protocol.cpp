#include "mailbox_protocol.hpp"
#include "host.hpp"
#include "mailbox_view.hpp"
#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>

namespace dfy::management {
namespace {
using Json = nlohmann::json;

// 限定不透明身份的类型与字节数，查找使用原值。
const std::string& identifier(const Json& value, std::size_t bytes) {
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size()>bytes) throw std::invalid_argument("mailbox: invalid identity");
    return text;
}

// 仅接收完整、规范的十进制修订，防止错误字段被当作一次成功的空读取。
std::uint64_t revision_number(const Json& value) {
    const auto& text = identifier(value,20);
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(),text.data()+text.size(),result);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || (text.size()>1 && text.front()=='0'))
        throw std::invalid_argument("mailbox: invalid revision");
    return result;
}

// 将公开地址按来源分组，过期后台返回独立的失效目录。
Json directory_rows(const app::MailboxDirectory& directory, bool stale) {
    auto sources = Json::array();
    if (!stale) for (const auto& [id,source] : directory.sources) {
        Json row{{"id",id},{"label",source.label},{"kind",source.plugin.empty() ? "builtin" : "plugin"},
            {"generation",std::to_string(source.generation)}};
        if (!source.plugin.empty()) row["plugin"] = source.plugin;
        sources.push_back(std::move(row));
    }
    return {{"event","mailbox.directory"},{"revision",std::to_string(directory.revision)},{"sampledUtcMs",directory.sampled_utc_ms},
        {"state",stale ? "stale" : "ready"},{"sources",std::move(sources)}};
}

// 将原信箱的入箱与实际取件时刻写进同一公开回执。
Json message_row(const events::MailboxMessageReceipt& row, const std::string& id, const char* direction) {
    Json receipt{{"mailbox",std::to_string(row.event.domain)},{"sequence",std::to_string(row.event.sequence)},
        {"state",row.received_utc_ms ? "received" : "accepted"},{"acceptedUtcMs",row.accepted_utc_ms},
        {"acceptedRevision",std::to_string(row.accepted_revision)}};
    if (row.received_utc_ms) {
        receipt["receivedUtcMs"] = *row.received_utc_ms;
        receipt["receivedRevision"] = std::to_string(row.received_revision);
    }
    return {{"id",id},{"category",row.category},{"categoryVersion",row.version},{"direction",direction},
        {"state",row.received_utc_ms ? "received" : "pending"},{"receipt",std::move(receipt)}};
}

// 从同一次发布读取一个来源，可用原消息号和来源修订复核选中项。
// 步骤：核对来源及修订 → 按地址列出真实水位 → 编码选中范围并按入箱顺序返回。
Json source_rows(const app::MailboxView& view, const std::string& id, bool wrong_instance,
    const std::string* selected, std::optional<std::uint64_t> expected) {
    Json result{{"event","mailbox.snapshot"},{"source",id},{"state","stale"},{"messages",Json::array()},{"mailboxes",Json::array()},
        {"revision",std::to_string(view.revision)},{"sampledUtcMs",view.sampled_utc_ms}};
    if (wrong_instance || !view.sources.contains(id)) return result;
    const auto& source = view.directory->sources.at(id);
    const auto& snapshot = *view.sources.at(id);
    result["revision"] = std::to_string(snapshot.revision);
    result["sampledUtcMs"] = snapshot.sampled_utc_ms;
    if (expected && *expected!=snapshot.revision) return result;

    auto addresses = Json::array();
    std::vector<std::pair<std::uint64_t,Json>> rows;
    for (std::size_t i=0; i<source.bindings.size(); ++i) {
        const auto& binding = source.bindings[i];
        const auto& box = snapshot.addresses[i];
        const char* direction = binding.sender==source.generation ? "toHost" : "toPlugin";
        const auto address = std::to_string(binding.mailbox.value);
        addresses.push_back({{"id",address},{"direction",direction},{"accepted",std::to_string(box.accepted)},
            {"received",std::to_string(box.received)},{"firstRetained",box.messages.empty() ? "0" : std::to_string(box.messages.front().event.sequence)}});
        for (const auto& message : box.messages) {
            const auto key = address+"/"+std::to_string(message.event.sequence);
            if (!selected || *selected==key) rows.emplace_back(message.accepted_revision,message_row(message,key,direction));
        }
    }
    if (selected && rows.empty()) return result;
    std::ranges::sort(rows,[](const auto& left, const auto& right) { return left.first>right.first; });
    for (auto& row : rows) result["messages"].push_back(std::move(row.second));
    result["mailboxes"] = std::move(addresses);
    result["state"] = "ready";
    return result;
}
}

// 将经过握手的管理请求限定于当前后台的不可变投影。
// 步骤：核对操作和字段 → 从同一采样读取目录或来源 → 回显请求及后台身份。
Json mailbox_request(const Host& host, const Json& request) {
    const auto& type = request.at("type");
    const bool bound = request.contains("instance"), selected = request.contains("message"), pinned = request.contains("revision");
    const bool stale = bound && identifier(request.at("instance"),64)!=host.instance();
    const auto view = host.mailboxes();
    if (!view) throw std::runtime_error("mailbox: directory unavailable");

    Json result;
    if (type=="mailbox.list" && (!pinned || bound) && request.size()==3+static_cast<unsigned>(bound)+static_cast<unsigned>(pinned)) {
        const bool changed = pinned && revision_number(request.at("revision"))!=view->directory->revision;
        result = directory_rows(*view->directory,stale || changed);
    } else if (type=="mailbox.read" && bound && request.contains("source") && request.size()==5+static_cast<unsigned>(selected)+static_cast<unsigned>(pinned)) {
        const auto& source = identifier(request.at("source"),256);
        const auto* message = selected ? &identifier(request.at("message"),48) : nullptr;
        const auto revision = pinned ? std::optional(revision_number(request.at("revision"))) : std::nullopt;
        result = source_rows(*view,source,stale,message,revision);
    } else throw std::invalid_argument("mailbox: invalid read request");
    result.update(Json{{"version","1"},{"request",request.at("request")},{"instance",host.instance()}});
    return result;
}
}
