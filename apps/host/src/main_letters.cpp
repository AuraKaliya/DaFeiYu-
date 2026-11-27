#include "main_messages.hpp"
#include "application.hpp"
#include "run_log.hpp"
#include <set>

namespace dfy::app {
using Json=nlohmann::json;
// 地址只在本次进程使用，重启恢复使用数据库中的原信号。
MainMessages::~MainMessages() {
    if (letter_sender_) app_.mailbox_host().network->revoke(letter_sender_);
}
// 宿主按原信号泵送最早待交付事件，实际信箱只携带指针。
// 步骤：定位待送信 → 懒注册可信接收地址 → 投递指针 → 在真实收件回调登记接收。
void MainMessages::advance_letters() {
    if (!letters_due_ || letter_in_flight_ || std::chrono::steady_clock::now()<retry_at_) return;
    try {
        const auto id=store_->pending_letter();
        if (!id) {letters_due_=false; return;}
        const auto& host=app_.mailbox_host(); constexpr auto category="memory.letter";
        if (!letter_box_.value) {
            const auto sender=runtime::next_domain(); auto box=host.network->open(host.identity,sender);
            const events::MailboxCategory wake[]={{category,1}};
            try {
                app_.mailbox_intake().watch(box,wake,[weak=weak_from_this()](const MailboxDelivery& delivery)->std::optional<MailboxProjection>{
                    const auto self=weak.lock();
                    if (!self) return {};
                    const auto* data=delivery.event.payload->find("text");
                    if (!data || data->as_string()!=std::to_string(self->letter_in_flight_))
                        throw std::logic_error("letter: wrong transport pointer");
                    self->store_->receive_letter(self->letter_in_flight_); self->letter_in_flight_=0;
                    return {};
                },MailboxPriority::normal,{},{},{},true);
            } catch (...) {host.network->revoke(sender); throw;}
            letter_sender_=sender; letter_box_=box;
        }
        const auto pointer=std::to_string(id);
        const auto receipt=host.network->deliver_next(letter_sender_,letter_box_,events::mailbox_text(category,1,pointer,host.network->payload_limit()));
        if (receipt.state==events::DeliveryState::accepted) {
            letter_in_flight_=id;
            if (trace_) trace_->mail("mail.accepted",letter_box_.value,letter_sender_,receipt.event,category,pointer);
        }
    } catch (const std::exception&) {
        storage_failed(); retry_at_=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
    }
}
// 私聊仅先接到待收提示，正文由主 Agent 在本轮预算中选取。
std::optional<agent::DecisionStatusSource> MainMessages::inbox_notice() const {
    if (!store_->unread_letter()) return {};
    static const std::string schema=R"([{"key":"inbox.pending","version":1,"description":"有子 Agent 来信待签收，可查 letters 的待收主题","defaultAttention":{"mode":"explicit"},"snapshotRead":false,"valueSchema":{"type":"object","properties":{"pending":{"type":"boolean"}},"additionalProperties":false}}])";
    return agent::DecisionStatusSource{"inbox",schema,Json::array({{{"key","inbox.pending"},{"version",1},{"subject","main"},{"fact",1},
        {"value",{{"pending",true}}}}}).dump()};
}
// 返回已入本体信箱的不可变快照，逐字原件要沿接收方副本号另取。
agent::DecisionStatusSource MainMessages::letter_source(std::uint64_t turn,const Json& result) const {
    static const auto schema=[]{
        Json fields=Json::object();
        for(const auto key : {"reference","plugin","subject","author","audience","channel","exchange","phase","mode","outcome","persona","startRef",
            "summary","summaryKind","bodyRef","noteRef","noteOrigin","letterRef","origin","text"}) fields[key]={{"type","string"}};
        for(const auto key : {"preset","nextOffset"}) fields[key]={{"type","integer"}};
        for(const auto key : {"acknowledged","partial"}) fields[key]={{"type","boolean"}};
        const Json item{{"type","object"},{"properties",fields},{"additionalProperties",false}};
        const Json value{{"type","object"},{"properties",{{"items",{{"type","array"},{"items",item}}},{"limited",{{"type","boolean"}}}}},{"additionalProperties",false}};
        return Json::array({{{"key","memory.letters"},{"version",1},{"description","发到本体的子 Agent 来信及明确导出的副本"},
            {"defaultAttention",{{"mode","explicit"}}},{"snapshotRead",false},{"valueSchema",value}}}).dump();
    }();
    return {"memory",schema,Json::array({{{"key","memory.letters"},{"version",1},{"subject",store_->conversation()},{"fact",turn},{"value",result}}}).dump()};
}
// 主 Agent 在读到私聊证据后才可确认这些原信，签收失败时保留待收状态。
void MainMessages::register_letter_tools() {
    reply_tools_->add({"main/memory_ack",app_.mailbox_host().identity,1,{"memory_ack",
        "签收本轮 memory.letters 中实际读过的信，references 填一至三个信号。签收保留原信，报告结果仍归发信者。",
        R"({"type":"object","properties":{"references":{"type":"array","items":{"type":"string"}}},"required":["references"],"additionalProperties":false})"},
        true,true,{},{},6,512,true},[this](const agent::ToolInvocation& call){
            try {
                if (!owner_memory_turn_ || !search_result_ || call.attempt.step.sequence<=search_step_) throw std::invalid_argument("unseen");
                const auto source=Json::parse(search_result_->samples).at(0);
                if (source.at("key")!="memory.letters") throw std::invalid_argument("unseen");
                const auto refs=Json::parse(call.call.arguments).at("references").get<std::vector<std::string>>();
                std::set<std::string> allowed;
                for(const auto& item : source.at("value").at("items")) if (item.contains("phase")) allowed.insert(item.at("reference"));
                for(const auto& ref : refs) if(!allowed.erase(ref)) throw std::invalid_argument("unseen");
                store_->acknowledge_letters(refs); return memory_receipt({{"state","acknowledged"}},agent::ToolEffect::applied);
            } catch(const std::invalid_argument&) {
                return memory_receipt({{"state","unseen_letter"}},agent::ToolEffect::not_applied,false);
            }
        });
}
}
