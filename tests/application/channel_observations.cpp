#include "task_memory_fixture.hpp"
#include "main_messages.hpp"
#include <fstream>
#include <iostream>
#include <map>

namespace dfy::tests::task_memory {
namespace {
using namespace dfy;
struct Source {std::string ref,tool,raw;};
// 建立两个独立的测试插件包，各自由自己的宿主工具身份签发回执。
fs::path package(const fs::path& root,const fs::path& library,const std::string& id) {
    const auto dest=root/"packages"/id; fs::create_directories(dest);
    const auto dll=library.filename().string(); fs::copy_file(library,dest/dll);
    Json manifest{{"manifest_version",1},{"id",id},{"name",id},{"version","1"},{"abi_version",2},{"entry",dll},{"default_config","record.json"}};
    std::ofstream(dest/"plugin.json")<<manifest.dump();
    return dest;
}
// 重启后走主人私聊检索，读取原任务回执或已发布的观察。
Json memory_items(const fs::path& data,const std::string& topic,std::size_t& chats,std::string_view view="tasks",std::string_view key="memory.tasks") {
    std::vector<model::Request> requests; Json items;
    app::ApplicationRunConfig config; config.trace_directory=data/"logs";
    app::Application host({},data/"empty",data,model::Backend{[&](const auto&,const model::Request& req)->model::Response {
        requests.push_back(req);
        if(requests.size()==1) {
            Json args{{"view",view},{"topics",{topic}},{"count",1},{"need","single"}};
            return {{model::Role::assistant,"",{{"original","memory_search",args.dump()}}},model::FinishReason::tool_calls};
        }
        const auto samples=Json::parse(req.messages.back().content).at("items");
        for(const auto& s:samples) if(s.at("key").get_ref<const std::string&>()==key) items=s.at("value").at("items");
        return {{model::Role::assistant,"已核对原件。"}};
    }},config);
    host.bind_messages(std::make_shared<app::MessageStore>(data),"original",nullptr);
    host.main_messages()->terminal("read-"+std::to_string(chats),"请展开这份原始回执。"); drain(host);
    require(requests.size()==2 && items.is_array(),"original request did not settle");
    trace(host.trace_file(),requests); chats+=requests.size(); return items;
}
// 每次重新启动本体，从普通私聊或房间问答核对后端实收、实际投递和原工具回执。
// 步骤：按语料提议一次检索 → 对照送模证据与原回执 → 校验引用随答复投递。
std::size_t recall(const fs::path& data,const Json& row,const std::map<std::string,Source>& originals) {
    auto db=std::make_shared<app::MessageStore>(data); std::vector<model::Request> requests;
    Json terms=Json::array(); for(const std::string t:row.at("topics"))
        terms.push_back(t.starts_with("@") ? originals.at(t.substr(1)).ref : t);
    std::string answer,failure;
    app::ApplicationRunConfig config; config.trace_directory=data/"logs";
    app::Application host({},data/"empty",data,model::Backend{[&](const auto&,const model::Request& req)->model::Response {
        requests.push_back(req);
        try {
            if(row.value("room",false)) {
                require(std::ranges::none_of(req.tools,[](const auto& tool){return tool.name=="memory_search";}),"room received private tool");
                for(const auto& m:req.messages) require(m.content.find("memory.observations")==m.content.npos,"room saw observations");
                answer="在房间里回复。";
            } else if(requests.size()==1) {
                Json args{{"view","observations"},{"topics",terms},{"need","single"},{"count",1}};
                return {{model::Role::assistant,"",{{"compare","memory_search",args.dump()}}},model::FinishReason::tool_calls};
            } else if (requests.size()==3 && row.contains("publishObservation")) {
                const auto receipt=std::ranges::find_if(req.messages.rbegin(),req.messages.rend(),[](const auto& m){return m.role==model::Role::tool;});
                require(receipt!=req.messages.rend() && receipt->tool_call_id=="catalogue-observation" &&
                    receipt->content.find(R"("state":"published")")!=std::string::npos,"direct observation was not published");
            } else {
                const auto tail=Json::parse(req.messages.back().content); const auto& samples=tail.at("items");
                const auto it=std::ranges::find_if(samples,[](const auto& s){return s.at("key")=="memory.observations";});
                require(it!=samples.end(),"missing observation projection");
                const auto& selected=it->at("value"); const auto& items=selected.at("items");
                require(items.dump().size()<=2048,"observation budget exceeded");
                if(!row.contains("state")) {
                    require(items.empty() && selected.value("ambiguous",false)==row.value("ambiguous",false),"unexpected evidence");
                    answer="目前没有可唯一定位的两份依据。";
                } else {
                    const auto& item=items.at(0); const auto& facts=item.at("facts");
                    require(items.size()==1 && facts.size()==2 && item.at("state")==row.at("state") &&
                        item.at("reason").get<std::string>().find(row.at("why").get<std::string>())!=std::string::npos,"wrong comparison or rationale");
                    answer=item.at("reason");
                    for(std::size_t i=0;i<2;++i) {
                        const auto& via=originals.at(row.at("via")[i]); const auto& root=originals.at(row.at("roots")[i]);
                        const auto& f=facts[i]; require(f.at("reference")==via.ref && f.at("readTool")==via.tool &&
                            f.at("originalRef")==root.ref && f.at("originalTool")==root.tool,"incorrect lineage");
                        const auto raw=Json::parse(root.raw);
                        if(f.contains("object")) {
                            for(const auto key:{"object","measure","label","checkpoint","sample","observer","value","unit"})
                                require(f.at(key)==raw.at(key),"value differs from original");
                            answer+=" "+f.at("object").get<std::string>()+" "+f.at("checkpoint").get<std::string>()+" "+
                                f.at("observer").get<std::string>()+" "+f.at("value").get<std::string>()+" "+f.at("unit").get<std::string>();
                            if(f.contains("normalizedValue")) answer+="（折合 "+f.at("normalizedValue").get<std::string>()+" "+f.at("normalizedUnit").get<std::string>()+"）";
                        }
                        if(row.contains("values")) require(f.at("normalizedValue")==row.at("values")[i] && f.at("normalizedUnit")==row.at("base"),"incorrect conversion");
                        answer+=" "+via.ref+" "+root.ref;
                    }
                    if (row.contains("publishObservation")) {
                        const auto& original=originals.at(row.at("publishObservation"));
                        Json args{{"library","observed"},{"reference",original.ref}};
                        return {{model::Role::assistant,"",{{"catalogue-observation","knowledge_observe",args.dump()}}},model::FinishReason::tool_calls};
                    }
                }
            }
        } catch(const std::exception& e) {failure=e.what(); answer="核对失败。";}
        return {{model::Role::assistant,answer}};
    }},config);
    host.bind_messages(db,"channels",nullptr);
    if(row.value("room",false)) {host.post_room_event(app::RoomEvent::addressed,"书房","两次读数怎么比较？"); host.advance_plugins();}
    else host.main_messages()->terminal(row.at("id"),row.at("ask"));
    drain(host); require(failure.empty(),failure);
    require(requests.size()==(row.value("room",false) ? 1u : row.contains("publishObservation") ? 3u : 2u),"wrong number of model requests");
    if(!row.value("room",false)) require(db->page(true,0,db->watermark(),1).records.front().envelope.at("text")==answer,"missing delivered citations");
    trace(host.trace_file(),requests); return requests.size();
}
}
// 用真实只读工具收取和转发原件，重启后按各自任务号核对，撤权后再查相同委托。
void channel_observations(const fs::path& corpus,const fs::path& data,const fs::path& freight,const fs::path& museum) {
    const std::map<std::string,std::string> ids{{"freight","freight.weighbridge"},{"museum","museum.inventory"}};
    package(data,freight,ids.at("freight")); package(data,museum,ids.at("museum"));
    std::map<std::string,Source> sources;
    std::ifstream stream(corpus,std::ios::binary); std::size_t tasks=0,chats=0,steps=0;
    for(std::string line;std::getline(stream,line);) {
        const auto row=Json::parse(line); const auto id=row.at("id").get<std::string>(); ++steps;
        try {
            if(row.contains("channel")) {
                const auto plugin=ids.at(row.at("channel")),tool=plugin+"/read"; const auto pkg=data/"packages"/plugin;
                auto record=row.value("record",Json::object());
                if(row.contains("forward")) {
                    const auto key=row.at("forward").get<std::string>(); record["forward"]=sources.contains(key) ? sources.at(key).ref : key;
                }
                std::ofstream(pkg/"record.json",std::ios::binary)<<record.dump();
                Json task{{"id","channel-"+std::to_string(steps)},{"tool",tool},{"readOnly",true},{"script",{"read","claim"}}};
                const auto goal=row.value("goal",id);
                const auto works=perform(task,data,pkg,goal,tasks);
                require(works.size()==1 && works[0].at("toolId")==tool,"wrong native tool");
                const auto raw=works[0].at("output").get<std::string>(); const auto envelope=Json::parse(raw);
                require(envelope.at("format")=="dfy.observation/1" && (record.contains("forward") ?
                    envelope.at("forward")==record.at("forward") : envelope.at("native")==record),"channel lost original data");
                const auto item=memory_items(data,goal,chats).at(0);
                const auto ref=item.at("receiptRef").get<std::string>();
                require(memory_items(data,ref,chats).at(0).at("payload")==raw,"could not reopen native receipt");
                sources[id]={ref,tool,raw};
            } else if (row.contains("recordObservation")) {
                app::Application host({},data/"empty",data); host.bind_messages(std::make_shared<app::MessageStore>(data),"admin",nullptr);
                bool rejected=false;
                try { host.main_messages()->manage_knowledge({"/knowledge","record-observation","observed",sources.at(row.at("recordObservation")).ref}); }
                catch (const std::invalid_argument&) {rejected=true;}
                require(rejected==row.value("deny",false),"wrong direct observation admission");
            } else if (row.contains("observationVisible")) {
                const auto items=memory_items(data,"交接箱",chats,"knowledge","knowledge.published");
                require(items.size()==(row.at("observationVisible")==true ? 1u : 0u),"wrong observation visibility after process recreation");
                if (!items.empty()) {
                    const auto& item=items.at(0);
                    require(item.at("applies")=="对象 assets:B7，度量 quantity:gross，检查点 audit:R1。" &&
                        item.at("statement")=="采样方 lab:balance，采样号 馆藏交接，原读数 1250 g。" &&
                        item.at("sources")==Json::array({{{"kind","channel_observation"},{"reference",sources.at("馆藏交接").ref}}}),"observation departed from native facts");
                }
            } else if(row.contains("withdraw")) {
                app::Application host({},data/"empty",data); host.bind_messages(std::make_shared<app::MessageStore>(data),"admin",nullptr);
                const auto ref=sources.at(row.at("withdraw")).ref; host.main_messages()->change_memory(ref.substr(0,ref.rfind("/receipt/")),std::nullopt);
            } else chats+=recall(data,row,sources);
        } catch(const std::exception& e) {throw std::runtime_error(id+": "+e.what());}
    }
    require(steps && tasks && chats,"empty channel corpus");
    std::cout<<steps<<" steps, "<<sources.size()<<" native receipts, "<<tasks<<" task requests, "<<chats<<" chat requests\n";
}
}
