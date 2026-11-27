#include "main_messages.hpp"
#include "task_reflection.hpp"

namespace dfy::app {
using Json=nlohmann::json;
// 主人声明可比性；事后整理的依据在查询中保留其标记。
void MainMessages::manage_case(const std::vector<std::string>& words) {
    if (words.size()==8 && words[1]=="define")
        store_->define_case(words[2],{{"object",words[3]},{"measure",words[4]},{"unit",words[5]},{"conditions",words[6]},{"criterion",words[7]}});
    else if (words.size()==5 && words[1]=="attach") store_->attach_case({words[2],words[3]},words[4]);
    else if (words.size()==4 && (words[1]=="alias" || words[1]=="unalias")) store_->case_alias(words[2],words[3],words[1]=="unalias");
    else throw std::invalid_argument("comparison: expected define, attach, alias or unalias");
    memory_revision_=store_->memory_revision(); if(reflection_ && words[1]=="attach") reflection_->interrupt();
}
// 只将已筛选原件和带作者的解释送给当前主人回合，工具历史仅记录命中数量。
agent::DecisionStatusSource MainMessages::case_source(std::uint64_t turn,const Json& result) const {
    static const auto schema=[] {
        const Json str{{"type","string"}},num{{"type","integer"}},flag{{"type","boolean"}};
        const auto obj=[](Json fields){return Json{{"type","object"},{"properties",std::move(fields)},{"additionalProperties",false}};};
        const auto list=[](const Json& element){return Json{{"type","array"},{"items",element}};};
        Json fields=Json::object();
        for(const auto k:{"reference","origin","checkpoint","attribution","actionTool","readTool","actionRef","receiptRef","expected","actual","effect",
            "author","exit","report","summaryKind"}) fields[k]=str;
        fields["met"]=flag; fields["partial"]=flag;
        const auto scope=obj({{"object",str},{"measure",str},{"unit",str},{"conditions",str},{"criterion",str}});
        const auto authored=obj({{"reference",str},{"author",str},{"standing",str},{"previousReference",str},{"judgement",str},{"text",str},{"cites",list(str)}});
        const auto item=obj({{"reference",str},{"scope",scope},{"state",str},{"nativeCount",num},{"reportCount",num},{"pathCount",num},{"nextOffset",num},
            {"facts",list(obj(fields))},{"opinion",authored},{"conflictRefs",list(str)}});
        return Json::array({{{"key","memory.comparisons"},{"version",2},{"description","按对象与适用条件选择的跨工具证据、大肥鱼的解释及需要澄清的歧义"},
            {"defaultAttention",{{"mode","explicit"}}},{"snapshotRead",false},{"valueSchema",obj({{"items",list(item)},{"limited",flag},{"ambiguous",flag}})}}}).dump();
    }();
    return {"memory",schema,Json::array({{{"key","memory.comparisons"},{"version",2},{"subject",store_->conversation()},{"fact",turn},{"value",result}}}).dump()};
}
}
