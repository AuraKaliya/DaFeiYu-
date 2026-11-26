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
    try { model::validate_request(input,window.thinking); }
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

// 核对宿主标记的普通问答边界，保留近期原件及供应商仍需要的独立推理消息。
std::size_t dialogue_span(const std::vector<model::Message>& messages, std::size_t i, bool keep_reasoning) {
    if (i+2+4 > messages.size() || messages[i].role != Role::user) return 0;
    const std::size_t span = messages[i+1].role == Role::user ? 3 : 2;
    if (i+span+4 > messages.size()) return 0;
    const auto& end = messages[i+span-1];
    if (end.role != Role::assistant || end.completed_dialogue_span != span || !end.tool_calls.empty() ||
        (keep_reasoning && end.reasoning_content)) return 0;
    return span;
}

// 按连续原始位置组合已完成的普通问答，返回仍有长回答可缩短的组位置。
// 步骤：跳过原始目标及保护消息 → 按完整轮次组成有界片段 → 写入有归属的历史视图。
std::vector<std::size_t> pack_dialogue(model::Request& input, const std::vector<std::size_t>& ordinals,
    std::uint64_t evidence_epoch, bool keep_reasoning) {
    const auto protected_end = input.messages.front().role == Role::system ? 3u : 2u;
    std::vector<model::Message> view;
    std::vector<std::size_t> large_groups;
    for (std::size_t i = 0; i < input.messages.size();) {
        if (i < protected_end || !dialogue_span(input.messages,i,keep_reasoning)) {
            view.push_back(std::move(input.messages[i++]));
            continue;
        }
        const auto first = i;
        bool large_answer = false;
        auto turns = nlohmann::json::array();
        while (const auto span = dialogue_span(input.messages,i,keep_reasoning)) {
            if (i-first+span > 64 || ordinals[i+span-1]-ordinals[first] != i-first+span-1) break;
            large_answer |= input.messages[i+span-1].content.size() > 1024;
            const auto& prompt = input.messages[i+span-2];
            const auto key = prompt.source == model::MessageSource::human || prompt.source == model::MessageSource::unknown
                ? std::string_view("user") : model::source_name(prompt.source);
            nlohmann::json turn{{std::string(key),prompt.content},{"assistant",input.messages[i+span-1].content}};
            if (span == 3) turn["host_status"] = input.messages[i].content;
            turns.push_back(std::move(turn));
            i += span;
        }
        if (i == first) { view.push_back(std::move(input.messages[i++])); continue; }
        nlohmann::json group{{"source","completed_dialogue"},{"epoch",evidence_epoch},{"first",ordinals[first]},
            {"count",i-first},{"turns",std::move(turns)}};
        if (large_answer) large_groups.push_back(view.size());
        view.push_back(model::sourced_message(model::MessageSource::host_summary,group.dump()));
    }
    input.messages = std::move(view);
    return large_groups;
}

// 只缩短宿主刚组成的旧问答组，保留原话、回答的两端和完整原件的位置。
// 步骤：按原轮次推进消息号 → 选择较长回答的完整 UTF-8 首尾 → 写回带原始字节数的预览。
void shorten_answers(model::Message& message) {
    auto group = nlohmann::json::parse(message.content);
    auto next = group.at("first").get<std::size_t>();
    for (auto& turn : group.at("turns")) {
        next += turn.contains("host_status") ? 3 : 2;
        const auto& answer = turn.at("assistant").get_ref<const std::string&>();
        if (answer.size() <= 1024) continue;
        std::size_t front = 192, back = answer.size()-192;
        while ((static_cast<unsigned char>(answer[front]) & 0xc0) == 0x80) --front;
        while ((static_cast<unsigned char>(answer[back]) & 0xc0) == 0x80) ++back;
        turn["assistant_preview"] = {{"prefix",answer.substr(0,front)},{"suffix",answer.substr(back)},
            {"bytes",answer.size()},{"message",next-1}};
        turn.erase("assistant");
    }
    message.content = group.dump();
}

// 为已结束的普通问答生成有原件引用的视图，在仍超限时逐组缩短旧长回答。
// 步骤：合并允许归档的整轮 → 检查逐字视图 → 从较旧回答起缩短到可送模。
bool fit_dialogue(model::Request& request, const std::vector<std::size_t>& ordinals, ContextWindow window, bool keep_reasoning) {
    const auto large_groups = pack_dialogue(request,ordinals,window.evidence_epoch,keep_reasoning);
    if (fits(request,window)) return true;
    for (auto index : large_groups) {
        shorten_answers(request.messages[index]);
        if (fits(request,window)) return true;
    }
    return false;
}
}
// 计算有界执行视图，按原始位置回查已结算工具批次和已完成的普通问答。
// 步骤：校验原始容量和输出预留 → 限制回执预览 → 按需归档整批工具及组合问答 → 核验最终请求。
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
    std::vector<std::size_t> ordinals;
    std::size_t compressed = 0;
    for (std::size_t i = 0; i < request.messages.size();) {
        ordinals.push_back(ordinal(i));
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
        compact.push_back(model::sourced_message(model::MessageSource::host_summary,ref.dump()));
        i = end; ++compressed;
    }
    request.messages = std::move(compact);
    if (compressed && fits(request,window)) return compressed;
    if (request.sends_reasoning(window.thinking) && std::ranges::any_of(request.messages,[](const auto& message) {
        return message.completed_dialogue_span && message.reasoning_content.has_value();
    })) {
        auto native = request;
        if (fit_dialogue(native,ordinals,window,true)) {
            request = std::move(native);
            return compressed;
        }
    }
    // 能独立归档的只有宿主标记的完整普通问答。工具续行和近期消息继续使用原生角色及推理字段。
    if (fit_dialogue(request,ordinals,window,false)) return compressed;
    throw std::length_error("context: model capacity exhausted before dispatch");
}
}
