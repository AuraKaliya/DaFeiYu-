#include "partner_loop.hpp"

namespace dfy::mc {
using Json = nlohmann::json;
namespace {
// 借用经宿主校验并由原收件引用持有的文本。
std::string_view borrowed(dfy_v2_string_view value) { return {value.data,value.length}; }
}

// 读取原发送方的一项请求，并按精确类别及版本分发。
// 步骤：取件及持有原引用 → 检查并读取信封 → 调用相应业务处理器。
void PartnerLoop::intake(PartnerWork work) {
    if (outbound_.size() > 120) return;
    dfy_v2_mailbox_message message{};
    if (mail_->poll(&message,nullptr)) return;
    struct Release {
        const dfy_v2_reference_api* refs;
        dfy_v2_handle payload;
        // 归还宿主交付的原收件引用。
        ~Release() { refs->release(payload,nullptr); }
    } release{refs_,message.payload};
    dfy_v2_mailbox_text text{};
    if (mail_text_->inspect(message.payload,&text,nullptr) || text.version != 1) return;
    const auto category = borrowed(text.category);
    if (category == "interaction.ask.present" || category == "interaction.ask.result" || category == "interaction.mail.reply")
        intake_question(category,Json::parse(borrowed(text.text),nullptr,false));
    if (category == "mc.partner.delegate" || category == "mc.partner.query")
        intake_task(category,Json::parse(borrowed(text.text),nullptr,false),work);
}

// 将本体委托纳入插件自己的循环；进度请求只返回已验证的现场和本地运行事实。
// 步骤：核对原身份和索引 → 接纳独立委托或读取原任务进度 → 返回经校验的回执。
void PartnerLoop::intake_task(std::string_view category, const Json& body, PartnerWork work) {
    if (!body.is_object() || !body.contains("id") || !body["id"].is_number_unsigned() || !body.contains("task") ||
        !body["task"].is_number_unsigned() || !body.contains("generation") || !body["generation"].is_string() ||
        !body.contains("player") || !body["player"].is_string() || !body.contains("text") || !body["text"].is_string()) return;
    const auto id = body.at("id").get<std::uint64_t>();
    const auto task = body.at("task").get<std::uint64_t>();
    const auto world = body.at("generation").get<std::string>();
    const auto prompt = body.at("text").get<std::string>();
    if (!id || !task || prompt.empty() || prompt.size() > 4096) return;
    const bool bound = feed_.ready() && feed_.cursor() && !feed_.departed() && world == feed_.world() && body["player"] == feed_.owner();
    if (category == "mc.partner.delegate") {
        if (id != task || id <= last_task_) return;
        if (!bound || awaiting_delegation_ || active_delegation_) {
            publish("mc.partner.report",{{"generation",world},{"id",id},{"ok",false},{"text","伙伴无法在当前世界接纳委托"}});
            return;
        }
        awaiting_delegation_ = Delegation{id,world,prompt};
        last_task_ = id;
        return;
    }
    const bool known = bound && task == last_task_;
    const std::string_view phase = active_delegation_ ? "running" : awaiting_delegation_ ? "queued" : "settled";
    Json state{{"task",task},{"phase",phase},{"order",feed_.cursor()}};
    if (active_delegation_ && work.invocation) { state["invocation"] = work.invocation; state["skill"] = work.skill; }
    auto result = std::string("伙伴无法核对原任务的当前进度");
    if (known) {
        state["observation"] = feed_.observation();
        result = state.dump();
        if (result.size() > 4096) { state.erase("observation"); state["observationOmitted"] = true; result = state.dump(); }
    }
    publish("mc.partner.answer",{{"generation",world},{"id",id},{"ok",known},{"text",result}});
}
}
