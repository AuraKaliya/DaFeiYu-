#include "main_messages.hpp"
#include "application.hpp"
#include <algorithm>

namespace dfy::app {
using Json = nlohmann::json;

// 由主 Agent 在实际发生选择时发言，后来需要比较时再按号取证。
void MainMessages::register_experience_tools() {
    reply_tools_->add({"main/choose",app_.mailbox_host().identity,1,{"main_choose",
        "主人让你当场做一项值得以后回顾的选择时，用此工具把你的选择实际发给主人。每份来信限一次。"
        "topic 是跨聊天稳定的事项。scene 逐字摘取影响选择的条件，options 逐字摘取主人本次提供的不同选项，choice 取其中之一。"
        "authority 逐字保留主人交代由谁决定的意思，自主选 free，明确指定选 directed，疑义选 unclear。"
        "impression 是愿意当面对主人说的简短感受或当时的猜想，原因未明可留空。回执 delivered 才算作出选择。"
        "转述、设想、拒绝保存、敏感内容跳过。reviewDue 时遇到反例、相似选择或主人追问，可查 episodes 对照。",
        R"({"type":"object","properties":{"topic":{"type":"string"},"scene":{"type":"string"},"options":{"type":"array","items":{"type":"string"}},"choice":{"type":"string"},"agency":{"type":"string","enum":["free","directed","unclear"]},"authority":{"type":"string"},"impression":{"type":"string"}},"required":["topic","scene","options","choice","agency","authority","impression"],"additionalProperties":false})"},
        true,true,{},{},4,1024,true},[this](const agent::ToolInvocation& call){return choose(call);});
    reply_tools_->add({"main/experience_review",app_.mailbox_host().identity,1,{"experience_review",
        "比较 memory_search 的 episodes 本轮装入的两至三次亲历，evidence 按实际发生顺序填 reference。"
        "后来的处境、感受或反例能解释过去时，kind 选 why，用 interpretation 简短说明你现在对当时原因的推测，保留不确定性。"
        "在同一适用条件、相同可选项下，前后有明确自选委托 openOffer 并作出了不同选择，kind 才选 change，interpretation 留空，宿主从实收推导变化。"
        "单次受命、意愿、重复讲述或只有解释变了都不足以写选择变化。两种判断可分别成立。",
        R"({"type":"object","properties":{"kind":{"type":"string","enum":["why","change"]},"evidence":{"type":"array","items":{"type":"string"}},"interpretation":{"type":"string"}},"required":["kind","evidence","interpretation"],"additionalProperties":false})"},
        true,true,{},{},5,512,true},[this](const agent::ToolInvocation& call){return review_experience(call);});
}

// 将实际选择与当场公开的感受写入同一投递，重试沿用原来信的投递身份。
std::unique_ptr<agent::ToolWork> MainMessages::choose(const agent::ToolInvocation& call) {
    if (!owner_memory_turn_ || parent_.first!=call.attempt.step.run)
        return memory_receipt({{"state","invalid_source"}},agent::ToolEffect::not_applied,false);
    const auto args = Json::parse(call.call.arguments);
    const auto text = "我选「"+args.at("choice").get<std::string>()+"」。"+args.at("impression").get<std::string>();
    try {
        auto receipt = reply(parent_.first,"choice-"+std::to_string(parent_.second),text,args);
        const bool delivered = receipt.at("state")=="delivered";
        if (delivered) {
            explicit_deliveries_[text] = receipt;
            const auto id = receipt.at("message").get<std::string>();
            receipt["reference"] = store_->conversation()+"/"+id;
            if (store_->receipt(std::stoll(id)).contains("reviewDue")) receipt["reviewDue"] = "compare_experiences";
        }
        return memory_receipt(std::move(receipt),delivered ? agent::ToolEffect::applied : agent::ToolEffect::unknown,delivered);
    } catch (const std::invalid_argument&) {
        return memory_receipt({{"state","invalid_choice"}},agent::ToolEffect::not_applied,false);
    }
}

// 每次只交付当前授权的实际经历，并将事后解释放在独立字段。
agent::DecisionStatusSource MainMessages::experience_source(std::uint64_t turn, const Json& selection) const {
    static const auto schema = [] {
        const Json str{{"type","string"}};
        Json fields = Json::object();
        for (const auto key : {"reference","parentReference","topic","scene","choice","agency","authority","impression","deliveredText","situationText"})
            fields[key] = str;
        fields["openOffer"] = {{"type","boolean"}};
        fields["utcMs"] = {{"type","integer"}};
        fields["options"] = {{"type","array"},{"items",str}};
        const Json episode{{"type","object"},{"properties",fields},{"additionalProperties",false}};
        const Json value{{"type","object"},{"properties",{{"items",{{"type","array"},{"items",episode}}},
            {"limited",{{"type","boolean"}}},{"kind",str},{"interpretation",str}}},{"required",{"items","limited"}},{"additionalProperties",false}};
        return Json::array({{{"key","memory.experiences"},{"version",1},{"description","本体已送达的选择、当场感受和有亲历依据的事后复盘"},
            {"defaultAttention",{{"mode","explicit"}}},{"snapshotRead",false},{"valueSchema",value}}}).dump();
    }();
    return {"memory",schema,Json::array({{{"key","memory.experiences"},{"version",1},{"subject",store_->conversation()},
        {"fact",turn},{"value",selection}}}).dump()};
}

// 逐项核实引用来自本轮更早决策中的亲历原件，写库前再核对授权与可比条件。
std::unique_ptr<agent::ToolWork> MainMessages::review_experience(const agent::ToolInvocation& call) {
    const auto args = Json::parse(call.call.arguments);
    if (!owner_memory_turn_ || !search_result_ || call.attempt.step.sequence<=search_step_)
        return memory_receipt({{"state","unseen_evidence"}},agent::ToolEffect::not_applied,false);
    const auto sample = Json::parse(search_result_->samples).at(0);
    const auto& items = sample.at("value").at("items");
    if (sample.at("key")!="memory.experiences" || sample.at("value").contains("kind") ||
        std::ranges::any_of(args.at("evidence"),[&](const Json& ref) {
            return std::ranges::none_of(items,[&](const Json& item){return item.at("reference")==ref;});
        })) return memory_receipt({{"state","unseen_evidence"}},agent::ToolEffect::not_applied,false);
    try {
        store_->review_experience(args);
    } catch (const std::invalid_argument&) {
        return memory_receipt({{"state","invalid_comparison"}},agent::ToolEffect::not_applied,false);
    }
    return memory_receipt({{"state","reviewed"},{"kind",args.at("kind")}},agent::ToolEffect::applied);
}
}
