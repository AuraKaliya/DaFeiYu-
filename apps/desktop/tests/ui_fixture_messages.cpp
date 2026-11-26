#include "ui_fixture_messages.hpp"
#include <iostream>
#include <vector>

namespace {
using namespace dfy::model;
using dfy::tests::fixture_business_messages;
constexpr char status_body[] = R"({"schema":"dafeiyu.agent_status/1","source":"host","scope":{"session":7,"epoch":2},"snapshot":17,"turn":3,"step":2,"asOf":{"event":40},"wakeEvents":[],"items":[]})";

// 固定请求未满足预期时带出具体消息选择错误。
void check(bool ok, const char* error) {
    if (!ok) throw std::runtime_error(error);
}

// 核对同一轮的原话、助手答复与补充，即使玩家发出与宿主尾注完全相同的 JSON 也保留。
// 步骤：核对旧版末条 → 附加有来源的同文尾注 → 核对原消息及其时间。
void dialogue() {
    std::vector<Message> messages{sourced_message(MessageSource::human,"等待桌面停止",501),
        {Role::assistant,"固定回复：等待桌面停止"},sourced_message(MessageSource::human,status_body,502)};
    for (bool with_status : {false,true}) {
        if (with_status) messages.push_back(sourced_message(MessageSource::host_status,status_body,700));
        const auto input = fixture_business_messages(messages);
        check(input.size()==3 && input.data()==messages.data(),"same-turn input includes the status trailer");
        check(input.front().content=="等待桌面停止" && input[1].role==Role::assistant,"previous dialogue changed");
        check(input.back().content==status_body && input.back().source==MessageSource::human &&
            input.back().event_utc_ms==502,"human body or original time changed");
    }
}

// 核对一批工具回执保留顺序、调用号和原采样时刻，固定模型仍读取最后的业务回执。
// 步骤：建立两个调用与回执 → 分别有无尾注取业务消息 → 核对时间和配对。
void tool_results() {
    Message first = sourced_message(MessageSource::tool,status_body,801);
    Message last = sourced_message(MessageSource::tool,"output:\nfocus",802);
    first.tool_call_id="read-a";
    last.tool_call_id="read-b";
    std::vector<Message> messages{{Role::assistant,"",{{"read-a","read","{}"},{"read-b","read","{}"}}},first,last};
    for (bool with_status : {false,true}) {
        if (with_status) messages.push_back(sourced_message(MessageSource::host_status,status_body,900));
        const auto input = fixture_business_messages(messages);
        check(input.size()==3 && input.data()==messages.data(),"tool result hidden by the status trailer");
        check(input.front().tool_calls[0].id==input[1].tool_call_id && input.front().tool_calls[1].id==input.back().tool_call_id,
            "tool call and result pairing changed");
        check(input[1].event_utc_ms==801 && input.back().event_utc_ms==802 &&
            input.back().role==Role::tool && input.back().content=="output:\nfocus","tool result or original time changed");
    }
}

// 旧状态、工具同文或缺少来源的用户正文保持原义，不能仅凭 JSON 判为尾注。
void other_messages() {
    auto tool = sourced_message(MessageSource::host_status,status_body);
    tool.role = Role::tool;
    for (const auto& message : {sourced_message(MessageSource::host_status,"旧状态"),
            sourced_message(MessageSource::host_status,R"({"schema":"other"})"),
            sourced_message(MessageSource::tool,status_body), tool, Message{Role::user,status_body}}) {
        const std::vector<Message> messages{message};
        check(fixture_business_messages(messages).size()==1,"non-trailer fixture message was discarded");
    }
    const std::vector<Message> history{sourced_message(MessageSource::host_status,status_body),
        sourced_message(MessageSource::human,"新的原话",1000)};
    check(fixture_business_messages(history).size()==2,"a non-final status message was removed");
}

// 空请求和仅有尾注的请求明确失败，以免固定模型凭空生成业务回复。
void missing_input() {
    std::vector<Message> messages;
    for (bool with_status : {false,true}) {
        if (with_status) messages.push_back(sourced_message(MessageSource::host_status,status_body));
        try { fixture_business_messages(messages); }
        catch (const std::runtime_error&) { continue; }
        throw std::runtime_error("missing business input passed the desktop fixture");
    }
}
}

// 在离线固定请求上验证桌面夹具消费新旧业务输入。
int main() {
    try { dialogue(); tool_results(); other_messages(); missing_input(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
