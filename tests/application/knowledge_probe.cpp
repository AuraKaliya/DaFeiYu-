#include "knowledge_probe.hpp"
#include "application_commands.hpp"
#include "main_messages.hpp"
#include <sstream>

namespace dfy::tests {
using task_memory::require;
// 使用实际接收的原件号，保留语料中独立指定的错误身份。
std::string KnowledgeProbe::resolve(const std::string& alias) const {
    const auto found=references.find(alias); return found==references.end() ? alias : found->second;
}
// 逐版核对受权文本及证据种类，子会话不能收到私人的原件地址。
void KnowledgeProbe::verify(const Json& items,const Json& wanted,bool owner) const {
    require(items.size()==wanted.size() && items.dump().size()<=2048,"knowledge result size");
    for(std::size_t i=0;i<wanted.size();++i) {
        const auto& v=items[i]; const auto& w=wanted[i];
        for(const auto key : {"article","edition","revision","applies","statement","proposer"}) if (w.contains(key))
            require(v.at(key)==w.at(key),std::string("incorrect publication ")+key);
        require(v.at("publisher")=="owner","unapproved publisher");
        const auto& sources=v.at("sources"); require(sources.size()==w.at("kinds").size(),"wrong number of knowledge sources");
        for(std::size_t j=0;j<sources.size();++j) {
            require(sources[j].at("kind")==w.at("kinds")[j] && sources[j].contains("reference")==owner,"private knowledge reference or wrong source");
            if (w.contains("refs") && owner) require(sources[j].at("reference")==resolve(w.at("refs")[j]),"wrong original");
        }
    }
}
// 走主聊天决策、工具和实际投递，不通过测试专用存储入口制造草案。
void KnowledgeProbe::chat(app::Application& host,const Json& input,unsigned id) {
    row=input; step=0; reply.clear(); host.main_messages()->terminal("kb"+std::to_string(id),"请按原来的依据核对这份做法。");
    task_memory::drain(host); require(!reply.empty(),"owner did not receive knowledge answer"); row=nullptr;
}
// 区分提出草案、读到发布版和真正发布三种动作。
model::Response KnowledgeProbe::answer(const model::Request& input) {
    const auto& terms=row.at("topics");
    if (++step==1) {
        require(std::ranges::any_of(input.tools,[](const auto& t){return t.name=="knowledge_propose";}) &&
            std::ranges::none_of(input.tools,[](const auto& t){return t.name=="knowledge_publish";}),"wrong owner chat authority");
        Json topics=Json::array(); for(const auto& term : terms) topics.push_back(resolve(term));
        Json search{{"topics",topics},{"view",row.value("view",std::string{"knowledge"})},{"need","multiple"},{"count",row.value("count",3)}};
        if (row.contains("offset")) search["offset"]=row.at("offset");
        return {{model::Role::assistant,"",{{"find","memory_search",search.dump()}}},model::FinishReason::tool_calls};
    }
    if (step==2) {
        const auto status=Json::parse(input.messages.back().content);
        const auto& samples=status.at("items");
        const auto kind=row.value("sample",std::string{"knowledge.published"});
        const auto found=std::ranges::find_if(samples,[&](const auto& s){return s.at("key")==kind;});
        require(found!=samples.end(),"expected knowledge source missing"); const auto& items=found->at("value").at("items");
        if (kind=="knowledge.published") verify(items,row.at("expect"),true);
        if (kind=="memory.comparisons") verify_comparison(items);
        if (row.contains("capture")) {
            const auto& save=row.at("capture"); references[save.at("alias").get<std::string>()]=items.at(0).at(save.at("field").get<std::string>());
        }
        if (row.contains("draft")) {
            auto draft=row.at("draft");
            for(auto& ref : draft.at("sources")) ref=resolve(ref);
            return {{model::Role::assistant,"",{{"offer","knowledge_propose",draft.dump()}}},model::FinishReason::tool_calls};
        }
    } else {
        const auto receipt=std::ranges::find_if(input.messages.rbegin(),input.messages.rend(),[](const auto& m){return m.role==model::Role::tool;});
        require(receipt!=input.messages.rend() && receipt->content.find(row.value("state",std::string{"recorded"}))!=std::string::npos,"wrong proposal outcome");
    }
    reply="已经按来源核对。"; return {{model::Role::assistant,reply}};
}
// 管理权限来自真实命令入口，失败操作必须拒绝，正文及修订可通过主人审阅结果复核。
void KnowledgeProbe::admin(app::Application& host,const Json& input) const {
    auto cmd=input.at("command").get<std::string>();
    for(auto it=references.rbegin();it!=references.rend();++it)
        if(auto p=cmd.find(it->first);p!=cmd.npos) cmd.replace(p,it->first.size(),it->second);
    std::ostringstream out,err;
    try { app::application_command(host,cmd,out,err); }
    catch(const std::invalid_argument&) {require(input.value("deny",false),"valid administrative action rejected: "+cmd); return;}
    require(!input.value("deny",false),"administrative action should have failed");
    if (!cmd.starts_with("/knowledge ")) return;
    const auto result=Json::parse(out.str());
    if (input.contains("revision")) require(result.at("revision")==input.at("revision"),"wrong publication revision");
    if (input.contains("inspect")) {
        const auto& item=result.at("editions").at(0);
        for(const auto& [k,v] : input.at("inspect").items()) require(item.at(k)==v,"incorrect audit detail");
        if (item.value("evidenceUnavailable",false)) require(!item.contains("statement"),"revoked content in review");
    }
}
}
