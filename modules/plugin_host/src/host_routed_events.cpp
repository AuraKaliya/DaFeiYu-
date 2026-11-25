#include "host_state.hpp"
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <deque>
#include <nlohmann/json.hpp>
#include <set>

namespace dfy::plugins::detail {
using Json = nlohmann::json;
struct EventRoutes {
    struct Schema {
        std::string type;
        std::uint32_t version;
        Json fields;
        dfy_v2_event_decoder decoder;
        void* context;
    };
    struct Item { std::size_t schema; std::uint64_t sequence; std::string payload; };
    runtime::DomainId domain = runtime::next_domain();
    std::uint64_t last = 0;
    bool decoding = false;
    std::vector<Schema> schemas;
    std::deque<std::shared_ptr<const Item>> pending, recent;
    std::vector<RoutedMailbox> mail;
};

namespace {
// 核对限定为命名空间格式的事件或分类标识。
bool namespaced(std::string_view name) {
    return name.find('.') != name.npos && name.front() != '.' && name.back() != '.' &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._") == name.npos;
}
// 读取受限字节视图，避免复制无效输入和超过声明上限的正文。
std::string_view bytes(dfy_v2_string_view value, std::size_t maximum) {
    if (!value.data || !value.length || value.length > maximum) throw std::invalid_argument("route: invalid bytes");
    return {value.data,static_cast<std::size_t>(value.length)};
}

// 读取闭合对象并拒绝重复字段及无界嵌套。
// 步骤：跟踪字段作用域 → 拒绝歧义或过深结构 → 核对根对象。
Json object_from(std::string_view text) {
    std::vector<std::set<std::string>> keys;
    const auto result = Json::parse(text,[&](int depth, Json::parse_event_t event, const Json& value) {
        if (depth > 12) throw std::invalid_argument("route: payload depth exceeded");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("route: duplicate field");
        return true;
    },false);
    if (!result.is_object()) throw std::invalid_argument("route: JSON object required");
    return result;
}

// 校验事件正文与预注册字段一一对应。
void require_schema(const Json& payload, const Json& fields) {
    if (payload.size() != fields.size()) throw std::invalid_argument("route: invalid field count");
    for (const auto& [key,kind] : fields.items()) {
        const auto field = payload.find(key);
        if (field == payload.end() || (kind == "string" && !field->is_string()) ||
            (kind == "uint" && !field->is_number_unsigned()) || (kind == "bool" && !field->is_boolean()))
            throw std::invalid_argument("route: invalid field");
    }
}

// 在插件启动时登记同一命名类型的精确版本。
// 步骤：校验类型及字段契约 → 核对启动阶段与容量 → 保存回调和调用上下文。
dfy_v2_status DFY_V2_CALL register_event(dfy_v2_string_view type, uint32_t version, dfy_v2_string_view schema,
    dfy_v2_event_decoder decoder, void* context, dfy_v2_error* error) {
    return boundary(error,[&](HostState& host) {
        if (host.event_routes_sealed || host.resources.state() != runtime::OwnerState::open) return DFY_V2_INVALID_CONTEXT;
        const auto name = bytes(type,96);
        if (!version || !decoder || !namespaced(name)) return DFY_V2_INVALID_ARGUMENT;
        auto fields = object_from(bytes(schema,4096));
        if (fields.size() > 24) return DFY_V2_RESOURCE_EXHAUSTED;
        for (const auto& [key,kind] : fields.items())
            if (key.empty() || (kind != "string" && kind != "uint" && kind != "bool")) return DFY_V2_INVALID_ARGUMENT;
        if (!host.event_routes) host.event_routes = std::make_shared<EventRoutes>();
        auto& routes = *host.event_routes;
        if (std::ranges::any_of(routes.schemas,[&](const auto& value) { return value.type == name && value.version == version; }))
            return DFY_V2_BUSY;
        if (routes.schemas.size() == 16) return DFY_V2_RESOURCE_EXHAUSTED;
        routes.schemas.push_back({std::string(name),version,std::move(fields),decoder,context});
        return DFY_V2_OK;
    });
}

// 核对并固定原实例提交的主信箱分类与策略建议。
// 步骤：验证注册阶段与类别 → 核对箱级优先级及重复 → 追加原类别。
dfy_v2_status DFY_V2_CALL register_mailbox(dfy_v2_string_view type, uint32_t version, uint32_t flags, dfy_v2_error* error) {
    return boundary(error,[&](HostState& host) {
        if (host.event_routes_sealed || host.resources.state() != runtime::OwnerState::open) return DFY_V2_INVALID_CONTEXT;
        const auto name = bytes(type,96);
        if (!namespaced(name) || name.starts_with("interaction.") || !version || flags & ~(DFY_V2_MAIL_HIGH | DFY_V2_MAIL_WAKE))
            return DFY_V2_INVALID_ARGUMENT;
        if (!host.event_routes) host.event_routes = std::make_shared<EventRoutes>();
        auto& mail = host.event_routes->mail;
        if (!mail.empty() && ((mail.front().flags ^ flags) & DFY_V2_MAIL_HIGH)) return DFY_V2_INVALID_ARGUMENT;
        if (std::ranges::any_of(mail,[&](const auto& entry) { return entry.category == name && entry.version == version; }))
            return DFY_V2_BUSY;
        if (mail.size() == 15) return DFY_V2_RESOURCE_EXHAUSTED;
        mail.push_back({std::string(name),version,flags});
        return DFY_V2_OK;
    });
}

// 按原序号接纳或复用一次事件，回执与解析分别结算。
// 步骤：匹配注册 → 核对重投和水位 → 校验载荷并提交队列 → 返回原身份。
dfy_v2_status DFY_V2_CALL emit(dfy_v2_string_view type, uint32_t version, uint64_t sequence,
    dfy_v2_string_view payload, dfy_v2_event_receipt* output, dfy_v2_error* error) {
    if (output) *output = {};
    return boundary(error,[&](HostState& host) {
        if (!output || !sequence) return DFY_V2_INVALID_ARGUMENT;
        if (!host.event_routes_sealed || !host.event_routes || host.event_routes->decoding ||
            host.resources.state() != runtime::OwnerState::open) return DFY_V2_INVALID_CONTEXT;
        auto& routes = *host.event_routes;
        const auto name = bytes(type,96), body = bytes(payload,DFY_V2_EVENT_ROUTE_CAPACITY);
        const auto schema = std::ranges::find_if(routes.schemas,[&](const auto& item) { return item.type == name && item.version == version; });
        if (schema == routes.schemas.end()) return DFY_V2_NOT_FOUND;
        const auto slot = static_cast<std::size_t>(schema-routes.schemas.begin());
        if (sequence <= routes.last) {
            const auto known = std::ranges::find_if(routes.recent,[&](const auto& item) { return item->sequence == sequence; });
            if (known == routes.recent.end()) return DFY_V2_NOT_FOUND;
            if ((*known)->schema != slot || (*known)->payload != body) return DFY_V2_INVALID_ARGUMENT;
            *output = {routes.domain,sequence,1,0};
            return DFY_V2_OK;
        }
        if (sequence != routes.last+1) return DFY_V2_INVALID_ARGUMENT;
        if (routes.pending.size() == 16) return DFY_V2_RESOURCE_EXHAUSTED;
        require_schema(object_from(body),schema->fields);
        auto item = std::make_shared<const EventRoutes::Item>(EventRoutes::Item{slot,sequence,std::string(body)});
        routes.pending.push_back(item);
        try { routes.recent.push_back(std::move(item)); }
        catch (...) { routes.pending.pop_back(); throw; }
        if (routes.recent.size() > 32) routes.recent.pop_front();
        routes.last = sequence;
        *output = {routes.domain,sequence,0,0};
        if (host.event_capture) try {
            host.event_capture(RoutedEvent{{routes.domain,sequence},0,{},std::string(name),std::string(body),{},version});
        } catch (...) {}
        return DFY_V2_OK;
    });
}
}

// 返回静态事件路由 ABI。
const dfy_v2_routed_event_api& routed_event_api() {
    static const dfy_v2_routed_event_api api{{sizeof(dfy_v2_routed_event_api),DFY_V2_ROUTED_EVENT_VERSION},register_event,emit};
    return api;
}

// 提供独立版本的可选信箱策略登记表。
const dfy_v2_routed_mailbox_api& routed_mailbox_api() {
    static const dfy_v2_routed_mailbox_api api{{sizeof(dfy_v2_routed_mailbox_api),1},register_mailbox};
    return api;
}
}

namespace dfy::plugins {
// 开始接纳事件并固定启动阶段的注册表。
void HostServices::seal_event_routes() { state_->require_owner(); state_->event_routes_sealed = true; }

// 在启动前绑定只观察已接纳事件的宿主记录器。
void HostServices::capture_events(EventCapture capture) {
    state_->require_owner();
    if (state_->event_routes_sealed) throw std::logic_error("events: capture already sealed");
    state_->event_capture = std::move(capture);
}

// 在原线程上调用已接纳事件的解析器，并校验投影及地址规则。
// 步骤：借用队首及注册 → 受保护调用并校验结果 → 消费原事件。
std::optional<RoutedEvent> HostServices::take_routed_event() {
    state_->require_owner();
    const auto routes = state_->event_routes;
    if (!routes || routes->pending.empty() || routes->decoding) return {};
    const auto& item = *routes->pending.front();
    const auto& schema = routes->schemas[item.schema];
    RoutedEvent result{{routes->domain,item.sequence},0,{},schema.type,item.payload,{},schema.version};
    const auto binding = bind();
    dfy_v2_event_projection output{};
    routes->decoding = true;
    try {
        result.status = schema.decoder(schema.context,{item.payload.data(),item.payload.size()},&output);
        if (!result.status) {
            if (output.kind < DFY_V2_EVENT_ASK || output.kind > DFY_V2_EVENT_ASK_RECEIPT || !output.length ||
                output.length > sizeof(output.json)) throw std::invalid_argument("route: invalid projection");
            const std::string_view projection(output.json,output.length);
            auto body = detail::object_from(projection);
            result.projection = projection;
            result.kind = output.kind;
            if (output.kind == DFY_V2_EVENT_MAIL || output.kind == DFY_V2_EVENT_ASK_VIA_MAIL) {
                const auto& mail = routes->mail;
                if (mail.size() == 1 && !body.contains("mailCategory") && !body.contains("mailVersion")) {
                    result.mailbox = mail.front();
                } else {
                    const auto category = body.at("mailCategory");
                    const auto version = body.at("mailVersion");
                    if (!category.is_string() || !version.is_number_unsigned())
                        throw std::invalid_argument("route: invalid mailbox category");
                    const auto found = std::ranges::find_if(mail,[&](const auto& rule) {
                        return category == rule.category && version == rule.version;
                    });
                    if (found == mail.end()) throw std::invalid_argument("route: mailbox category not registered");
                    result.mailbox = *found;
                    body.erase("mailCategory");
                    body.erase("mailVersion");
                    result.projection = body.dump();
                }
                result.mailbox_categories = mail;
            }
        }
    } catch (...) { result.status = DFY_V2_FAILED; }
    routes->decoding = false;
    routes->pending.pop_front();
    return result;
}
}
