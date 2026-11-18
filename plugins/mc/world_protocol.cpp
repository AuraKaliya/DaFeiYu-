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

// 只接收固定本机通道配置，错误配置在实例建立前失败。
std::string configured_channel(std::string_view config) {
    const auto value = parse(config);
    const auto channel = value.at("channel").get<std::string>();
    if (value.size() != 1 || !valid_channel(channel)) throw std::invalid_argument("MC protocol: channel");
    return channel;
}

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
}
