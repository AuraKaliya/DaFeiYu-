#include <dafeiyu/context_capacity.hpp>
#include <dafeiyu/model_protocol.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>

namespace dfy::agent {
namespace {
using model::Role;
// 统计本地持有的原始字节，模型窗口和协议硬限稍后对视图单独核算。
std::size_t history_bytes(const model::Request& input) {
    std::size_t sum = 0;
    for (const auto& m : input.messages) {
        sum += m.content.size()+m.tool_call_id.size();
        if (m.reasoning_content) sum += m.reasoning_content->size();
        for (const auto& c : m.tool_calls) sum += c.id.size()+c.name.size()+c.arguments.size();
    }
    return sum;
}
// 检查完整编码的硬限及已配置的保守窗口，输出预留按实际配置扣减一次。
bool fits(const model::Request& input, ContextWindow window) {
    if (input.messages.size() > 1024) return false;
    try { model::validate_request(input); }
    catch (const std::length_error&) { return false; }
    if (!window.tokens) return true;
    // 用编码字节保守估算输入 token，另留消息封套和供应商模板余量。
    const auto bytes = model::encode_request_fields(input,window.thinking).size();
    return bytes+64+8*input.messages.size() <= window.tokens-window.output;
}
// 将超长回执改成有字节上限的完整 UTF-8 预览，并保留原号及原消息位置。
void limit_receipt(model::Message& message, std::uint64_t epoch, std::size_t ordinal) {
    const auto limit = message.receipt_view_bytes;
    if (!limit || message.content.size() <= limit) return;
    const auto ref = "\n[host_evidence epoch="+std::to_string(epoch)+" message="+std::to_string(ordinal)+
        " bytes="+std::to_string(message.content.size())+"]";
    if (limit < 256 || ref.size() >= limit) throw std::invalid_argument("context: invalid receipt view size");
    auto bytes = limit-ref.size();
    while ((static_cast<unsigned char>(message.content[bytes]) & 0xc0) == 0x80) --bytes;
    message.content.resize(bytes); message.content += ref;
}
}
// 计算有界执行视图，只有宿主已经确认的完整旧批次可以变成可回查的证据引用。
// 步骤：校验原始容量和输出预留 → 缩短有明确上限的回执 → 按需整批归档 → 核验最终请求。
std::size_t fit_context_request(model::Request& request, ContextWindow window, int head_delta) {
    if ((window.tokens || window.output) && (!window.output || window.output >= window.tokens || window.tokens > 16*1024*1024))
        throw std::invalid_argument("context: invalid configured model window");
    if (head_delta < -1 || head_delta > 1 || request.messages.size() > 8192 || history_bytes(request) > 8*1024*1024)
        throw std::length_error("context: retained evidence limit exceeded");
    if (window.output) request.max_output_tokens = window.output;
    const auto ordinal = [head_delta](std::size_t i) { return std::size_t(static_cast<std::ptrdiff_t>(i)+1-head_delta); };
    for (std::size_t i = 0; i < request.messages.size(); ++i) if (request.messages[i].role == Role::tool)
        limit_receipt(request.messages[i],window.evidence_epoch,ordinal(i));
    if (fits(request,window)) return 0;

    std::vector<model::Message> compact;
    std::size_t compressed = 0;
    for (std::size_t i = 0; i < request.messages.size();) {
        const auto& assistant = request.messages[i];
        const auto count = assistant.tool_calls.size();
        const auto end = i+1+count;
        const bool eligible = assistant.role == Role::assistant && count && end+4 <= request.messages.size() &&
            std::all_of(request.messages.begin()+i+1,request.messages.begin()+end,[](const auto& m) {
                return m.role == Role::tool && m.settled_evidence;
            });
        if (!eligible) { compact.push_back(std::move(request.messages[i++])); continue; }

        model::Request batch;
        batch.messages.assign(request.messages.begin()+i,request.messages.begin()+end);
        model::validate_request(batch);
        auto ids = nlohmann::json::array();
        for (const auto& call : assistant.tool_calls) ids.push_back(call.id);
        const auto ref = nlohmann::json{{"source","settled_tool_batch"},{"epoch",window.evidence_epoch},
            {"first",ordinal(i)},{"count",count+1},{"call_ids",std::move(ids)}};
        compact.push_back({Role::user,ref.dump()});
        i = end; ++compressed;
    }
    request.messages = std::move(compact);
    if (!fits(request,window)) throw std::length_error("context: model capacity exhausted before dispatch");
    return compressed;
}
}
