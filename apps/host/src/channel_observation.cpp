#include "message_store_db.hpp"
#include <algorithm>
#include <charconv>

namespace dfy::app {
using namespace message_db;
using Json=nlohmann::json;
namespace {
struct Receipt {
    std::string reference,reader;
    std::int64_t task;
    Json payload;
};
// 将完整任务名或本档案的规范回执号定位到唯一一次成功的只读采样。
// 步骤：确定任务身份 → 核对实际派发与成功退出 → 取只读任务的读口或变更任务的最终核验。
std::optional<Receipt> read_receipt(sqlite3* db,const std::string& profile,const std::string& topic,bool& ambiguous) {
    const auto prefix=profile+"/task/";
    const bool named=topic.find("/task/")==topic.npos;
    std::int64_t task=0;
    if(named) {
        Statement find(db,"SELECT id FROM task_memory WHERE active=1 AND goal=? LIMIT 2");
        find.bind(1,topic); if(!find.next()) return {};
        task=find.number(0); if(find.next()) {ambiguous=true; return {};}
    } else {
        if(!topic.starts_with(prefix)) return {};
        const auto tail=std::string_view(topic).substr(prefix.size());
        const auto [end,ec]=std::from_chars(tail.data(),tail.data()+tail.size(),task);
        if(ec!=std::errc{} || task<=0 || !std::string_view(end,tail.data()+tail.size()).starts_with("/receipt/")) return {};
    }
    Statement get(db,"SELECT t.body,coalesce(o.readback,0) FROM task_memory t LEFT JOIN task_observation o ON o.task=t.id WHERE t.id=? AND t.active=1");
    get.bind(1,task); if(!get.next()) return {};
    const auto body=Json::parse(get.text(0));
    if(!body.contains("works") || !body.contains("changes")) return {};
    std::optional<Receipt> result;
    for(const auto& work:body.at("works")) {
        if(!work.value("dispatched",false) || !work.value("succeeded",false) || !work.contains("result") ||
            !work.contains("toolId") || !work.at("toolId").is_string() || !work.contains("output") || !work.at("output").is_string()) continue;
        const auto candidate=work.at("candidate").get<std::int64_t>();
        if(body.at("changes")==true ? candidate!=get.number(1) : work.at("toolId")!=body.at("tool")) continue;
        const auto ref=prefix+std::to_string(task)+"/receipt/"+std::to_string(candidate);
        if(!named && ref!=topic) continue;
        if(result) {ambiguous=true; return {};}
        const auto& raw=work.at("output").get_ref<const std::string&>();
        result=Receipt{ref,work.at("toolId"),task,raw.size()<=4096 ? Json::parse(raw,nullptr,false) : Json(nullptr)};
    }
    return result;
}
// 协议标识必须含有签发方命名空间，本地名称仅用于展示。
bool field(const Json& p,const char* key,bool qualified=false) {
    if(!p.contains(key) || !p.at(key).is_string() || p.at(key).dump().size()>66) return false;
    const auto& s=p.at(key).get_ref<const std::string&>(); const auto pos=s.find(':');
    if(s.empty() || s.find_first_not_of(" \t\r\n")==s.npos) return false;
    return !qualified || (pos!=s.npos && pos!=0 && pos+1<s.size() && s.find(':',pos+1)==s.npos &&
        s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:._-")==s.npos);
}
// 以十进制移位得到精确的毫克或毫米，并消除前导零与无意义的小数尾零。
std::optional<std::pair<std::string,std::string>> normalize(const std::string& value,const std::string& unit) {
    struct Unit {const char* name; const char* base; unsigned shift;};
    static constexpr Unit units[]={{"kg","mg",6},{"g","mg",3},{"mg","mg",0},{"m","mm",3},{"cm","mm",1},{"mm","mm",0}};
    const auto it=std::ranges::find(units,unit,&Unit::name);
    const auto dot=value.find('.');
    if(it==std::end(units) || value.empty() || value.size()>24 || dot==0 || value.back()=='.' ||
        value.find_first_not_of("0123456789.")!=value.npos || (dot!=value.npos && value.find('.',dot+1)!=value.npos)) return {};
    auto digits=value;
    auto places=dot==value.npos ? 0u : static_cast<unsigned>(value.size()-dot-1);
    if(dot!=value.npos) digits.erase(dot,1);
    if(places<=it->shift) digits.append(it->shift-places,'0');
    else {
        places-=it->shift; digits.insert(digits.size()-places,1,'.');
        while(digits.back()=='0') digits.pop_back();
        if(digits.back()=='.') digits.pop_back();
    }
    const auto first=digits.find_first_not_of('0');
    if(first==digits.npos) digits="0";
    else digits.erase(0,digits[first]=='.' ? first-1 : first);
    return std::pair<std::string,std::string>{digits,it->base};
}
// 验证转发只引用此前签发的一份直接读回，引用和原件每次查回都重新裁权。
std::optional<Json> evidence(sqlite3* db,const std::string& profile,Receipt delivery) {
    Json f{{"reference",delivery.reference},{"readTool",delivery.reader}};
    if(delivery.payload.is_object() && delivery.payload.value("format",Json{})=="dfy.observation/1" && delivery.payload.contains("forward")) {
        if(delivery.payload.size()!=2 || !delivery.payload.at("forward").is_string()) return {};
        const auto ref=delivery.payload.at("forward").get<std::string>();
        if(!ref.starts_with(profile+"/task/") || ref.size()>160) return {};
        bool ambiguous=false; auto source=read_receipt(db,profile,ref,ambiguous);
        if(!source || source->task>=delivery.task || source->payload.contains("forward")) return {};
        delivery=std::move(*source);
    }
    f["originalRef"]=delivery.reference; f["originalTool"]=delivery.reader;
    const auto& p=delivery.payload;
    if(!p.is_object() || p.value("format",Json{})!="dfy.observation/1" || p.contains("forward") ||
        !field(p,"object",true) || !field(p,"measure",true) || !field(p,"observer",true) || !field(p,"checkpoint",true) ||
        !field(p,"sample") || !field(p,"label") || !field(p,"unit") || !field(p,"value")) return f;
    for(const auto key:{"object","measure","observer","checkpoint","sample","label","unit","value"}) f[key]=p.at(key);
    if(auto n=normalize(p.at("value"),p.at("unit"))) {f["normalizedValue"]=n->first; f["normalizedUnit"]=n->second;}
    return f;
}
// 同次采样先查来源与对象，再核对检查点、量纲和精确值；共同来源始终只算一份依据。
std::pair<const char*,const char*> decide(const Json& a,const Json& b) {
    if(a.at("originalRef")==b.at("originalRef")) return {"same_receipt","两个引用追到同一份原回执，只计一次。"};
    if(!a.contains("object") || !b.contains("object")) return {"unverified","读回缺少可核验的采样字段。"};
    if(a.at("observer")==b.at("observer") && a.at("sample")==b.at("sample")) {
        if(a.at("object")==b.at("object") && a.at("measure")==b.at("measure") && a.at("checkpoint")==b.at("checkpoint") && a.contains("normalizedUnit") &&
            b.contains("normalizedUnit") && a.at("normalizedUnit")==b.at("normalizedUnit") && a.at("normalizedValue")==b.at("normalizedValue"))
            return {"same_sample","两个渠道记录的是同一采样方的同一采样号，只计一次。"};
        return {"inconsistent_source","同一采样方的同一采样号给出了无法核对的内容，须追查原件。"};
    }
    if(a.at("object")!=b.at("object")) return {"different_objects","两个对象的带命名空间标识不同，不能合并。"};
    if(a.at("measure")!=b.at("measure")) return {"different_measures","原件登记的度量不同，同一单位也不能直接对照。"};
    if(a.at("checkpoint")!=b.at("checkpoint")) return {"different_checkpoints","两份原件的采样检查点不同，不能判定同一时点的状态。"};
    if(!a.contains("normalizedUnit") || !b.contains("normalizedUnit") || a.at("normalizedUnit")!=b.at("normalizedUnit"))
        return {"incompatible_units","单位、量纲或数值不能精确换算，保留原读数。"};
    if(a.at("observer")==b.at("observer") || a.at("originalTool")==b.at("originalTool"))
        return {"shared_source","两个读回共用采样方或原读口，独立性不足。"};
    if(a.at("normalizedValue")==b.at("normalizedValue")) return {"equivalent","同一对象和检查点，两份独立采样换算到共同单位后数值相同。"};
    return {"conflict","同一对象和检查点，两份独立采样换算后仍相矛盾，须保留双方原件。"};
}
}
// 在同一档案快照中重查两项原件与转发来源，按一项比较交付双方，保持顺序与预算。
Json MessageStore::compare_observations(const std::vector<std::string>& topics) const {
    if(topics.size()!=2) throw std::invalid_argument("observations: expected two task names or receipts");
    Json out{{"items",Json::array()},{"limited",false}}, facts=Json::array();
    std::lock_guard lock(state_->mutex); auto* db=state_->db;
    // 保留读取游标直至本次比较结束，使两侧原件及其授权处于同一个 SQLite 读事务。
    Statement snapshot(db,"SELECT id FROM conversation"); snapshot.next();
    for(const auto& topic:topics) {
        if(topic.empty() || topic.size()>160) throw std::invalid_argument("observations: invalid source");
        bool ambiguous=false; auto r=read_receipt(db,conversation(),topic,ambiguous);
        if(!r) {if(ambiguous) out["ambiguous"]=true; return out;}
        auto f=evidence(db,conversation(),std::move(*r)); if(!f) return out;
        facts.push_back(std::move(*f));
    }
    const auto [state,reason]=decide(facts[0],facts[1]);
    Json item{{"state",state},{"reason",reason},{"facts",std::move(facts)}};
    if(item.dump().size()+2>2048) {
        // 先留双方原件和判定，再成对补回同一字段，避免长标识挤掉另一方出处。
        const auto full=std::move(item.at("facts")); item["facts"]=Json::array(); item["partial"]=true;
        for(const auto& f:full) item["facts"].push_back({{"reference",f.at("reference")},{"originalRef",f.at("originalRef")}});
        for(const auto key:{"object","measure","checkpoint","value","unit","normalizedValue","normalizedUnit","observer","sample","originalTool","readTool","label"}) {
            for(std::size_t i=0;i<2;++i) if(full[i].contains(key)) item["facts"][i][key]=full[i].at(key);
            if(item.dump().size()+2>2048) for(auto& f:item["facts"]) f.erase(key);
        }
    }
    out["items"].push_back(std::move(item)); return out;
}
}
