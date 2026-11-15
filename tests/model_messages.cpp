#include <dafeiyu/model_messages.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::model;
// 拒绝非法协议输入，任何正常返回都视为测试失败。
void rejects(const Request& request) {
    try { validate_request(request); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("invalid model request accepted");
}
// 构造两项调用及对应结果的固定对话，不调用任何模型或工具。
Request fixture() {
    return {{{Role::system,"rules"},{Role::user,"goal"},
        {Role::assistant,"",{{"a","read_state","{}"},{"b","read_state","{\"target\":1}"}}},
        {Role::tool,"first",{},"a"},{Role::tool,"second",{},"b"}},
        {{"read_state","Read a state","{\"type\":\"object\"}"}},32};
}
// 验证结构化消息、参数与配对边界，普通文本接口仍可使用相同消息类型。
// 步骤：接受固定完整对话 → 拒绝损坏的角色和配对 → 拒绝错误定义与容量。
void protocol() {
    validate_request(fixture()); validate_request({{{Role::user,"你好"},{Role::assistant,"回答"}}});
    auto repeated = fixture();
    repeated.messages.push_back({Role::assistant,"",{{"a","read_state","{}"}}});
    repeated.messages.push_back({Role::tool,"new step",{},"a"});
    validate_request(repeated);
    auto request = fixture(); request.messages.pop_back(); rejects(request);
    request = fixture(); request.messages[4].tool_call_id = "a"; rejects(request);
    request = fixture(); request.messages[2].tool_calls[1].id = "a"; rejects(request);
    request = fixture(); request.messages[3].role = Role::user; rejects(request);
    request = fixture(); request.messages[1].role = Role::system; rejects(request);
    request = fixture(); request.messages[1].role = static_cast<Role>(99); rejects(request);
    request = fixture(); request.messages[2].tool_calls[0].arguments = "{incomplete"; rejects(request);
    request = fixture(); request.messages[2].tool_calls[0].arguments = "[]"; rejects(request);
    request = fixture(); request.tools.push_back(request.tools.front()); rejects(request);
    request = fixture(); request.tools[0].name = "name.with.dot"; rejects(request);
    request = fixture(); request.tools[0].parameters = "[]"; rejects(request);
    request = fixture(); request.messages[1].content.assign(1024*1024,'x'); rejects(request);
    request = fixture(); request.messages.assign(1025,{Role::user,"x"}); rejects(request);
    request = fixture(); request.max_output_tokens = 0; rejects(request);
    request = fixture(); request.messages[1].content = std::string(1,static_cast<char>(0xff)); rejects(request);
    request = fixture(); request.messages[2].tool_calls[0].id = std::string(1,static_cast<char>(0xff)); rejects(request);
}
// 拒绝不能交给执行层的模型响应，不执行其中的任何工具。
void rejects_response(const Response& response) {
    try { validate_response(response); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("invalid model response accepted");
}
// 验证完整调用与文本兼容，并保证截断、重复和结束原因不一致时不交付调用。
// 步骤：接受正常文本与调用 → 拒绝残缺和重复调用 → 保留无调用的受限结束原因。
void responses() {
    validate_response({{Role::assistant,"answer"}});
    const Response call{{Role::assistant,"",{{"one","read_state","{}"}}},FinishReason::tool_calls};
    validate_response(call);
    auto response = call; response.finish = FinishReason::length; rejects_response(response);
    response = call; response.finish = FinishReason::stop; rejects_response(response);
    response = call; response.message.tool_calls.clear(); rejects_response(response);
    response = call; response.message.tool_calls.push_back(response.message.tool_calls.front()); rejects_response(response);
    response = call; response.message.tool_calls[0].arguments = "{\"incomplete\":"; rejects_response(response);
    response = call; response.message.role = Role::user; rejects_response(response);
    response = call; response.message.content.assign(8*1024*1024,'x'); rejects_response(response);
    rejects_response({{Role::assistant,""}});
    rejects_response({{Role::assistant,std::string(1,static_cast<char>(0xff))}});
    validate_response({{Role::assistant,"partial"},FinishReason::length});
    validate_response({{Role::assistant,""},FinishReason::content_filter});
}
}
// 执行纯内存的模型消息校验并报告结果。
int main() {
    try { protocol(); responses(); std::cout << "Model message checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
