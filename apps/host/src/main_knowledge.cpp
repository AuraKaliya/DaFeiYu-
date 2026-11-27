#include "knowledge.hpp"
#include "main_messages.hpp"
#include "application.hpp"
#include <charconv>
#include <set>

namespace dfy::app {
using Json=nlohmann::json;
// 同一知识契约服务于主人和受权子库；引用字段仅在存储层授权后出现。
agent::DecisionStatusSource knowledge_source(const std::string& subject,std::uint64_t fact,const Json& selection) {
    static const auto schema=[]{
        const Json str{{"type","string"}},number{{"type","integer"}};
        Json fields=Json::object();
        for(const auto key : {"article","edition","topic","applies","statement","proposer","publisher"}) fields[key]=str;
        fields["revision"]=number;
        fields["sources"]={{"type","array"},{"items",{{"type","object"},{"properties",{{"kind",str},{"reference",str}}},{"additionalProperties",false}}}};
        const Json item{{"type","object"},{"properties",fields},{"additionalProperties",false}};
        const Json value{{"type","object"},{"properties",{{"items",{{"type","array"},{"items",item}}},{"limited",{{"type","boolean"}}}}},{"additionalProperties",false}};
        return Json::array({{{"key","knowledge.published"},{"version",1},{"description","主人发布的当前知识版本，含适用条件、修订和来源类别"},
            {"defaultAttention",{{"mode","explicit"}}},{"snapshotRead",false},{"valueSchema",value}}}).dump();
    }();
    return {"knowledge",schema,Json::array({{{"key","knowledge.published"},{"version",1},{"subject",subject},{"fact",fact},{"value",selection}}}).dump()};
}
// 只允许主 Agent 从本轮已交付的依据提议，发布、改变读者和撤下属于主人。
void MainMessages::register_knowledge_tool() {
    reply_tools_->add({"main/knowledge_propose",app_.mailbox_host().identity,1,{"knowledge_propose",
        "主人要求整理可复用做法时，可将刚刚查到的真实依据整理成待主人审阅的草案。"
        "article、edition 为简短字母数字键，base 为当前发布修订，首次为 0。说明 topic、applies、statement。"
        "sources 填本轮看过的一至三份主人原话号、动作后独立读回号或子 Agent 结束信号。报告须注明原作者。成功后仅说明已提议。",
        R"({"type":"object","properties":{"article":{"type":"string"},"edition":{"type":"string"},"base":{"type":"integer"},"topic":{"type":"string"},"applies":{"type":"string"},"statement":{"type":"string"},"sources":{"type":"array","items":{"type":"string"}}},"required":["article","edition","base","topic","applies","statement","sources"],"additionalProperties":false})"},
        true,true,{},{},7,512,true},[this](const agent::ToolInvocation& call){
            try {
                if (!owner_memory_turn_ || !search_result_ || call.attempt.step.sequence<=search_step_) throw std::invalid_argument("unseen");
                const auto source=Json::parse(search_result_->samples).at(0);
                const auto& kind=source.at("key");
                std::set<std::string> seen;
                for(const auto& item : source.at("value").at("items")) {
                    if (kind=="memory.matches" || (kind=="memory.letters" && item.value("phase",std::string{})=="end") ||
                        (kind=="memory.tasks" && item.value("kind",std::string{})=="receipt")) seen.insert(item.at("reference"));
                    if (kind=="memory.tasks" && item.contains("readbackRef")) seen.insert(item.at("readbackRef"));
                    if (kind=="memory.understanding") for(const auto& f : item.at("facts")) seen.insert(f.at("readbackRef"));
                }
                const auto draft=Json::parse(call.call.arguments);
                for(const auto& ref : draft.at("sources")) if(!seen.contains(ref.get<std::string>())) throw std::invalid_argument("unseen");
                return memory_receipt(store_->propose_knowledge(draft,"main"),agent::ToolEffect::applied);
            } catch(const std::invalid_argument&) {return memory_receipt({{"state","invalid_proposal"}},agent::ToolEffect::not_applied,false);}
        });
}
namespace {
// 管理命令中的版本号须完整解析，防止错误撤下其他修订。
std::int64_t revision(const std::string& value) {
    std::int64_t n=0; const auto [end,ec]=std::from_chars(value.data(),value.data()+value.size(),n);
    if(ec!=std::errc{} || end!=value.data()+value.size() || n<0) throw std::invalid_argument("knowledge: invalid revision");
    return n;
}
}
// 主人逐版决定哪些草案可发布、给谁读；变更后撤销旧请求并隔离原上下文。
Json MainMessages::manage_knowledge(const std::vector<std::string>& words) {
    if (words.size()<3) throw std::invalid_argument("knowledge: expected propose, inspect, publish, withdraw or grant");
    const auto& verb=words[1],&article=words[2];
    if (verb=="inspect" && (words.size()==3 || words.size()==4)) return store_->inspect_knowledge(article,words.size()==4 ? words[3] : "");
    if (verb=="propose" && words.size()>=9 && words.size()<=11) {
        Json draft{{"article",article},{"edition",words[3]},{"base",revision(words[4])},{"topic",words[5]},
            {"applies",words[6]},{"statement",words[7]},{"sources",std::vector<std::string>(words.begin()+8,words.end())}};
        return store_->propose_knowledge(draft,"owner");
    }
    if ((verb=="publish" || verb=="withdraw") && words.size()==4) {
        auto withdraw=verb=="withdraw" ? std::optional(revision(words[3])) : std::nullopt;
        if (store_->publish_knowledge(article,words[3],withdraw)) {
            memory_revision_=store_->memory_revision(); search_result_.reset();
            if (app_.conversation().busy() && owner_memory_turn_) app_.conversation().cancel();
            app_.subagents().invalidate_knowledge(article);
        }
        return store_->inspect_knowledge(article,"");
    }
    if (verb=="grant" && words.size()==9) {
        const auto preset=words[5]=="task" ? plugins::MemoryPreset::task : words[5]=="business" ? plugins::MemoryPreset::business :
            words[5]=="avatar" ? plugins::MemoryPreset::avatar : throw std::invalid_argument("knowledge: invalid preset");
        const int rights=words[8]=="off" ? 0 : words[8]=="read" ? 1 : words[8]=="relay" ? 5 : -1;
        const auto child=store_->grant_knowledge(article,words[3],words[4],{preset,words[6],words[7]=="-" ? "" : words[7]},rights);
        if (child) app_.subagents().invalidate_knowledge(article,child);
        return {{"state","configured"},{"article",article},{"edition",words[3]}};
    }
    throw std::invalid_argument("knowledge: invalid command arguments");
}
}
