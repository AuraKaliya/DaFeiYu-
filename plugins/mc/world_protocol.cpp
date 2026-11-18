#include "world_protocol.hpp"
#include "pipe_exchange.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <vector>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;
// 在解释消息前限制字节、嵌套层数和同层重复键。
// 步骤：检查输入大小 → 按嵌套对象核对键 → 生成完整 JSON 值。
Json parse(std::string_view text) {
    if (text.empty() || text.size() > 16384) throw std::invalid_argument("MC protocol: size");
    std::vector<std::set<std::string>> objects;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 6) throw std::invalid_argument("MC protocol: depth");
        if (event == Json::parse_event_t::object_start) objects.emplace_back();
        else if (event == Json::parse_event_t::key && !objects.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("MC protocol: duplicate field");
        else if (event == Json::parse_event_t::object_end) objects.pop_back();
        return true;
    });
}
// 验证由宿主和游戏来源使用的规范 UUID。
bool uuid(const Json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}
// 验证维度是可显示的 Minecraft 资源名称。
bool dimension(const Json& value) {
    if (!value.is_string()) return false;
    const auto& name = value.get_ref<const std::string&>();
    return !name.empty() && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || std::string_view(":/._-").find(c) != std::string_view::npos;
    });
}
}

// 读取独立通道及可选私有配对键，错误配置在实例建立前失败。
BridgeConfig configured_bridge(std::string_view config) {
    const auto value = parse(config);
    const auto channel = value.at("channel").get<std::string>();
    if (!valid_channel(channel) || value.size() != (value.contains("bridgeKey") ? 2u : 1u))
        throw std::invalid_argument("MC protocol: channel");
    std::string key;
    if (value.contains("bridgeKey")) {
        key = value.at("bridgeKey").get<std::string>();
        if (key.size() != 64 || key.find_first_not_of("0123456789abcdef") != key.npos)
            throw std::invalid_argument("MC protocol: invalid private configuration");
    }
    return {channel, key};
}
// 仅取公开通道名。
std::string configured_channel(std::string_view config) { return configured_bridge(config).channel; }

// 在工具层生成查询，模型只能提供空参数。
std::string world_request(std::string_view id) {
    return Json{{"version", 1}, {"method", "state.read"}, {"id", id}}.dump();
}

// 校验原查询对应的世界观测，继续保留客户端数量和服务端信号年龄的语义。
// 步骤：核对信封和身份 → 处理已撤销世界 → 校验字段类型与范围并返回值副本。
std::string world_reply(std::string_view wire, std::string_view expected_id) {
    const auto reply = parse(wire);
    if (reply.size() != 3 || !reply.at("version").is_number_unsigned() ||
        reply.at("version") != 1 || !reply.at("id").is_string() || reply.at("id").get_ref<const std::string&>() != expected_id)
        throw std::invalid_argument("MC protocol: wrong reply");
    const auto& state = reply.at("state");
    if (state.is_null()) return "null";

    if (state.size() != 6 || !uuid(state.at("generation")) || !uuid(state.at("actor")) ||
        !dimension(state.at("dimension")) || !state.at("cobblestone").is_number_unsigned() || state.at("cobblestone") > 9007199254740991ULL ||
        !state.at("signalAgeMs").is_number_unsigned() || state.at("signalAgeMs") > 9007199254740991ULL)
        throw std::invalid_argument("MC protocol: wrong world state");
    const auto& pos = state.at("position");
    if (pos.size() != 3) throw std::invalid_argument("MC protocol: wrong position");
    for (const auto* key : {"x", "y", "z"})
        if (!pos.at(key).is_number() || !std::isfinite(pos.at(key).get<double>()))
            throw std::invalid_argument("MC protocol: wrong coordinate");
    return state.dump();
}

// 从受控参数建立私有玩家消息查询。
std::string inbox_request(std::string_view id, std::string_view key, std::string_view arguments) {
    auto request = parse(arguments);
    if (key.size() != 64 || request.size() != 2 || !uuid(request.at("generation")) ||
        !request.at("after").is_number_unsigned() || request.at("after") > 9007199254740991ULL)
        throw std::invalid_argument("MC protocol: invalid inbox query");
    request.update(Json{{"version", 1}, {"id", id}, {"key", key}, {"method", "chat.read"}});
    return request.dump();
}

// 校验世界、绑定玩家、窗口缺口和每条原话的序号。
// 步骤：匹配原请求 → 验证保留窗口 → 检查消息数量、文本与续读标记。
std::string inbox_reply(std::string_view wire, std::string_view id, std::string_view arguments) {
    const auto reply = parse(wire), query = parse(arguments);
    if (reply.size() != 3 || !reply.at("version").is_number_unsigned() || reply.at("version") != 1 ||
        reply.at("id").get_ref<const std::string&>() != id) throw std::invalid_argument("MC protocol: wrong private reply");
    const auto& inbox = reply.at("inbox");
    if (inbox.is_null()) return "null";
    if (inbox.size() != 7 || inbox.at("generation") != query.at("generation") || !uuid(inbox.at("owner")) ||
        !inbox.at("oldest").is_number_unsigned() || !inbox.at("latest").is_number_unsigned() ||
        !inbox.at("gap").is_boolean() || !inbox.at("more").is_boolean()) throw std::invalid_argument("MC protocol: inbox shape");
    const auto oldest = inbox.at("oldest").get<std::uint64_t>(), latest = inbox.at("latest").get<std::uint64_t>();
    const auto after = query.at("after").get<std::uint64_t>();
    if (latest > 9007199254740991ULL || !oldest || (latest ? oldest > latest : oldest != 1) || after > latest ||
        inbox.at("gap") != (after < oldest - 1)) throw std::invalid_argument("MC protocol: inbox cursor");

    const auto& messages = inbox.at("messages");
    if (!messages.is_array() || messages.size() > 8) throw std::invalid_argument("MC protocol: inbox size");
    auto cursor = std::max(after, oldest - 1);
    for (const auto& message : messages) {
        if (message.size() != 2 || !message.at("sequence").is_number_unsigned() || message.at("sequence") != ++cursor ||
            !message.at("text").is_string()) throw std::invalid_argument("MC protocol: inbox message");
        const auto& text = message.at("text").get_ref<const std::string&>();
        if (text.empty() || text.size() > 1024) throw std::invalid_argument("MC protocol: inbox text");
    }
    if (cursor > latest || (cursor < latest && messages.empty()) || inbox.at("more") != (cursor < latest))
        throw std::invalid_argument("MC protocol: inbox continuation");
    return inbox.dump();
}

// 为同一逻辑动作生成接纳、查询或停止帧，不在参数中接收私有键或方法。
std::string inspect_request(std::string_view method, std::string_view id, std::string_view key, std::string_view arguments) {
    auto frame = parse(arguments);
    if (key.size() != 64 || frame.size() != 3 || !uuid(frame.at("generation")) || !frame.at("order").is_number_unsigned() ||
        frame.at("order") == 0 || frame.at("order") > 9007199254740991ULL || !frame.at("place").is_string() ||
        !valid_channel(frame.at("place").get_ref<const std::string&>())) throw std::invalid_argument("MC protocol: inspect target");
    if (method != "inspect.begin") {
        if (method != "inspect.read" && method != "inspect.stop") throw std::invalid_argument("MC protocol: inspect control");
        frame.erase("place");
    }
    frame.update(Json{{"version", 1}, {"id", id}, {"key", key}, {"method", method}});
    return frame.dump();
}

// 只接受与原委托一致的动作状态，到达时石料可以是明确未看见的 null。
// 步骤：核对请求与目标 → 校验状态及退出语义 → 验证位置和可见方块读回。
InspectReply inspect_reply(std::string_view wire, std::string_view id, std::string_view arguments) {
    const auto reply = parse(wire), args = parse(arguments);
    if (reply.size() != 3 || !reply.at("version").is_number_unsigned() || reply.at("version") != 1 ||
        reply.at("id").get_ref<const std::string&>() != id) throw std::invalid_argument("MC protocol: inspect reply");
    const auto& action = reply.at("action");
    if (action.at("generation") != args.at("generation") || !action.at("order").is_number_unsigned() ||
        action.at("order") != args.at("order") || action.at("place") != args.at("place") ||
        !action.at("exited").is_boolean() || !action.at("moved").is_boolean()) throw std::invalid_argument("MC protocol: wrong action");
    const auto& phase = action.at("phase").get_ref<const std::string&>();
    const bool exited = action.at("exited"), arrived = phase == "arrived";
    if ((exited && !arrived && phase != "stopped" && phase != "failed") ||
        (!exited && phase != "moving" && phase != "stopping") || action.size() != (arrived ? 8u : 6u))
        throw std::invalid_argument("MC protocol: inconsistent action phase");
    if (arrived) {
        const auto& position = action.at("position");
        const auto& stone = action.at("stone");
        if (position.size() != 3 || (!stone.is_null() && stone.size() != 3)) throw std::invalid_argument("MC protocol: observation");
        for (auto axis : {"x", "y", "z"}) {
            if (!position.at(axis).is_number() || !std::isfinite(position.at(axis).get<double>()) ||
                (!stone.is_null() && !stone.at(axis).is_number_integer())) throw std::invalid_argument("MC protocol: observation coordinates");
        }
    }
    return {exited, arrived, action.at("moved").get<bool>(), action.dump()};
}
}
