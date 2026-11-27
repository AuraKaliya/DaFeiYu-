#include "message_protocol.hpp"
#include "host.hpp"
#include "message_store.hpp"
#include <charconv>

namespace dfy::management {
namespace {
using Json = nlohmann::json;
// 读取含零的非负十进制游标，拒绝溢出和尾随字符。
std::int64_t cursor(const Json& value) {
    const auto& text = value.get_ref<const std::string&>();
    std::int64_t result = 0;
    const auto parsed = std::from_chars(text.data(),text.data()+text.size(),result);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || result<0)
        throw std::invalid_argument("messages: invalid cursor");
    return result;
}
// 将数据库身份编码成十进制串，并保留该修订对应的真实回执。
Json wire(const app::StoredMessage& row) {
    auto result = row.envelope;
    result.update(Json{{"message",std::to_string(row.id)},{"revision",std::to_string(row.revision)},{"utcMs",row.utc_ms},
        {"source",row.source},{"submission",row.submission},{"receipt",row.receipt}});
    return result;
}
}

// 每个窗口独立持有订阅资格与本地游标，读取不改动原消息和工作。
// 步骤：登记并采样当前水位 → 核对逻辑会话 → 查询原提交或补读固定边界。
Json message_request(Host& host, const Json& request, const std::string& client) {
    auto* db = host.messages();
    if (!db) throw std::invalid_argument("messages: storage unavailable");
    const auto& type = request.at("type");
    const bool subscribe = type=="messages.subscribe";
    if (subscribe && request.size()!=3) throw std::invalid_argument("messages: invalid subscription");
    const auto high = host.message_watermark(client,subscribe);
    if (!high) throw std::invalid_argument("messages: subscribe first");
    Json reply{{"version","1"},{"request",request.at("request")},{"conversation",db->conversation()},
        {"watermark",std::to_string(*high)},{"instance",host.instance()}};
    if (subscribe) {
        reply["event"] = "messages.subscribed";
        return reply;
    }
    if (request.at("conversation")!=db->conversation()) throw std::invalid_argument("messages: different conversation");
    if (type=="messages.tasks" && request.size()==4) {
        reply["event"] = "messages.tasks";
        reply["tasks"] = db->recent_tasks();
        return reply;
    }
    if (type=="messages.receipt" && request.size()==6) {
        const auto row = db->find(request.at("source").get_ref<const std::string&>(),request.at("submission").get_ref<const std::string&>());
        reply["event"] = "messages.receipt";
        reply["record"] = row ? wire(*row) : Json(nullptr);
        return reply;
    }
    if (type!="messages.read" || request.size()!=8) throw std::invalid_argument("messages: unknown request");
    const bool history = request.at("direction")=="history";
    if (!history && request.at("direction")!="changes") throw std::invalid_argument("messages: invalid direction");
    const auto index = cursor(request.at("cursor")), through = cursor(request.at("through"));
    const auto& limit = request.at("limit");
    if (!limit.is_number_integer() || limit<=0 || limit>64) throw std::invalid_argument("messages: invalid page size");
    const auto page = db->page(history,index,through,limit.get<unsigned>());
    auto records = Json::array();
    auto next = index;
    for (const auto& row : page.records) {
        next = history ? row.id : row.revision;
        records.push_back(wire(row));
    }
    reply.update(Json{{"event","messages.page"},{"through",std::to_string(through)},{"next",std::to_string(next)},
        {"more",page.more},{"records",std::move(records)}});
    return reply;
}
}
