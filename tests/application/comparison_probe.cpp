#include "knowledge_probe.hpp"
#include <set>

namespace dfy::tests {
using task_memory::require;
// 语料给预期来源，原读数和动作号始终从插件实际发出的 Trace 取值。
void KnowledgeProbe::verify_comparison(const Json& items) {
    require(items.dump().size()<=2048,"comparison budget exceeded");
    if (row.value("missing",false)) {require(items.empty(),"withdrawn or foreign evidence exposed"); return;}
    require(items.size()==1,"comparison unavailable"); const auto& v=items.at(0);
    for (const auto key:{"state","nativeCount","reportCount","pathCount","nextOffset","scope"})
        if(row.contains(key)) require(v.at(key)==row.at(key),std::string("wrong comparison ")+key);
    const auto& facts=v.at("facts"); std::set<std::string> actual;
    for(const auto& f : facts) {
        const auto ref=f.at("reference").get<std::string>(); actual.insert(ref);
        if(f.at("origin")=="main_readback") {
            const auto original=std::ranges::find_if(traces,[&](const auto& it){return resolve(it.first)==ref;});
            require(original!=traces.end(),"unrecognized readback"); const auto& works=original->second;
            const auto& action=works.at(1), &reader=works.at(2);
            const auto base=ref.substr(0,ref.rfind("/receipt/")),num=std::to_string(action.at("candidate").get<std::uint64_t>());
            require(f.at("actual")==reader.at("output") && f.at("receiptRef")==base+"/receipt/"+num && f.at("actionRef")==base+"/action/"+num &&
                ref==base+"/receipt/"+std::to_string(reader.at("candidate").get<std::uint64_t>()) && f.at("actionTool")==action.at("toolId") &&
                f.at("readTool")==reader.at("toolId") && f.at("met")==Json(f.at("expected")==f.at("actual")),"mismatched original tool evidence");
        } else require(f.at("origin")=="sender_report" && f.at("author").get<std::string>().find("/child/")!=std::string::npos &&
            !f.contains("met") && !f.contains("actionRef"),"report presented as a main action");
    }
    if (row.contains("facts")) {
        std::set<std::string> wanted; for(const auto& ref : row.at("facts")) wanted.insert(resolve(ref));
        require(actual==wanted,"wrong comparison witnesses");
    }
    if (row.contains("attributes")) for(const auto& [ref,want] : row.at("attributes").items()) {
        const auto it=std::ranges::find_if(facts,[&](const auto& f){return f.at("reference")==resolve(ref);});
        require(it!=facts.end(),"specific witness missing");
        for(const auto& [k,val] : want.items()) require(it->at(k)==val,"incorrect original attribution or text");
    }
    if (row.contains("conflict")) {
        std::set<std::string> expected; for(const auto& r : row.at("conflict")) expected.insert(resolve(r));
        require(v.at("conflictRefs").get<std::set<std::string>>()==expected,"wrong independent disagreement");
    } else require(!v.contains("conflictRefs"),"different sampling checkpoints conflict");
    if (row.contains("opinion")) {
        const auto& o=v.at("opinion");
        require(o.at("author")=="main","wrong interpretation author");
        for(const auto& [k,val] : row.at("opinion").items()) {
            if(k=="save") references[val.get<std::string>()]=o.at("reference");
            else if(k=="previousReference") require(o.at(k)==resolve(val),"wrong predecessor");
            else require(o.at(k)==val,"incorrect authored interpretation");
        }
        require(o.at("cites").size()>=2,"missing opinion citations");
    } else require(!v.contains("opinion"),"unexpected authored opinion");
}
}
