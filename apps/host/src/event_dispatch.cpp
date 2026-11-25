#include "event_dispatch.hpp"
#include "interaction_routes.hpp"
#include "run_log.hpp"
#include "transition_notice.hpp"
#include <dafeiyu/routed_event_abi_v2.h>
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
using Json = nlohmann::json;
// 读取投影中的无符号身份，未开票的终态允许票号为零。
std::uint64_t route_id(const Json& body, const char* field, bool allow_zero = false) {
    const auto& value = body.at(field);
    if (!value.is_number_unsigned() || (!allow_zero && value == 0)) throw std::invalid_argument("event: invalid route identity");
    return value.get<std::uint64_t>();
}

// 将插件 ABI 投影转为应用内部指令。
// 步骤：读取投影对象 → 按处理类型校验字段 → 固定类型化指令。
EventRoute parse_route(const plugins::RoutedEvent& event) {
    const auto body = Json::parse(event.projection);
    switch (event.kind) {
    case DFY_V2_EVENT_MAIL: return MailRoute{};
    case DFY_V2_EVENT_ASK:
    case DFY_V2_EVENT_ASK_VIA_MAIL: {
        const auto wait = route_id(body,"waitMs");
        const auto channel = body.at("channel").get<std::string>();
        if (wait > 120000 || (channel != "game" && channel != "terminal")) throw std::invalid_argument("event: invalid ASK route");
        return AskRoute{route_id(body,"item"),route_id(body,"revision"),wait,body.at("question"),body.at("recipient"),channel,
            body.value("choices",std::vector<std::string>{}),event.kind == DFY_V2_EVENT_ASK_VIA_MAIL};
    }
    case DFY_V2_EVENT_ASK_FEEDBACK: {
        const auto action = body.at("action").get<std::string>();
        if (action != "presented" && action != "answer") throw std::invalid_argument("event: invalid ASK feedback");
        return AskFeedback{route_id(body,"ask"),body.at("channel"),body.value("recipient",std::string{}),body.value("answer",std::string{}),
            action == "presented" ? AskFeedback::Action::presented : AskFeedback::Action::answer,body.value("delivered",false)};
    }
    case DFY_V2_EVENT_ASK_RECEIPT: {
        const auto disposition = body.at("disposition").get<std::string>();
        if (disposition != "matched" && disposition != "stale") throw std::invalid_argument("event: invalid ASK receipt");
        events::EventId message{};
        if (event.version != 1 || body.contains("mailbox") || body.contains("message"))
            message = {route_id(body,"mailbox"),route_id(body,"message")};
        return AskReceipt{{route_id(body,"eventDomain"),route_id(body,"event")},route_id(body,"ask",true),route_id(body,"item"),
            route_id(body,"revision"),disposition,message};
    }
    default: throw std::invalid_argument("event: unknown route");
    }
}
}

// 分别记录原生解析及应用分派结果。
// 步骤：借用原实例解析结果 → 验证类型化指令 → 分派并记账。
void dispatch_event(plugins::PluginManager& plugins, InteractionRoutes& interactions, RunLog* log) {
    auto event = plugins.take_routed_event();
    if (!event) return;
    const auto id = event->id;
    if (log) log->append("event.decoded",{{"eventDomain",id.domain},{"event",id.sequence},{"plugin",event->plugin},
        {"instance",event->owner},{"type",event->type},{"status",event->status},{"route",event->kind},{"body",event->payload}});
    notify_transition(plugins,log,{DFY_V2_TRANSITION_EVENT_DECODED,event->owner,id,event->type,event->status});
    if (event->status) return;
    TransitionNotice dispatched{DFY_V2_TRANSITION_EVENT_DISPATCHED,event->owner,id,event->type,DFY_V2_FAILED};
    try {
        auto route = parse_route(*event);
        const auto kind = event->kind;
        const bool optional = std::holds_alternative<MailRoute>(route) || std::holds_alternative<AskRoute>(route);
        const auto gate = plugins.transition_hook({DFY_V2_TRANSITION_EVENT_BEFORE_DISPATCH,
            optional ? DFY_V2_TRANSITION_CAN_DENY : 0,kind,0,event->owner,id.domain,id.sequence,0,0,0,
            {event->type.data(),event->type.size()},{}});
        if (log && gate.observed) log->append("hook.transition",{{"point","event.before_dispatch"},{"eventDomain",id.domain},
            {"event",id.sequence},{"version",gate.version},{"allowed",gate.allowed},{"errors",gate.errors}});
        const bool accepted = interactions.dispatch(std::move(*event),std::move(route),!gate.allowed);
        dispatched.status = accepted ? DFY_V2_OK : DFY_V2_FAILED;
        if (log) log->append("event.dispatched",{{"eventDomain",id.domain},{"event",id.sequence},{"route",kind},{"accepted",accepted}});
    } catch (const std::exception&) {
        if (log) log->append("event.rejected",{{"eventDomain",id.domain},{"event",id.sequence}});
    }
    notify_transition(plugins,log,dispatched);
}
}
