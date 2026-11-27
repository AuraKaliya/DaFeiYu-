#include "task_memory_fixture.hpp"
#include "main_messages.hpp"
#include "message_store_db.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/model_protocol.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>

namespace dfy::tests::task_memory {
namespace {
using namespace app::message_db;
using Clock=std::chrono::steady_clock;
struct Measures {
    unsigned cases=0, requests=0, challenges=0;
    std::size_t max_evidence=0, max_wire=0;
    std::vector<double> roundtrips;
};

// 构造旧版档案格式的独立检索对照，两个目标组与不同规模的相近干扰组共用同一查询入口。
void populate(const fs::path& data,unsigned groups) {
    const auto profile=app::MessageStore(data).conversation();
    sqlite3* handle=nullptr;
    const auto path=(data/"main-conversation.sqlite3").u8string();
    check(sqlite3_open(reinterpret_cast<const char*>(path.c_str()),&handle));
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> close(handle,&sqlite3_close);
    Transaction tx(handle);
    sql(handle,"DROP TABLE task_understanding; CREATE TABLE task_understanding(id INTEGER PRIMARY KEY,family TEXT NOT NULL,"
        "parent INTEGER NOT NULL,through_task INTEGER NOT NULL,relation TEXT NOT NULL,claim TEXT NOT NULL,cites TEXT NOT NULL,"
        "contrast INTEGER NOT NULL,valid INTEGER NOT NULL DEFAULT 1); CREATE INDEX understanding_series ON task_understanding(family,through_task);"
        "PRAGMA user_version=7; UPDATE conversation SET memory_revision=1");
    for (unsigned group=1;group<=groups;++group) {
        const auto tool=group==1 ? std::string{"library/arrange"} : group==2 ? "gallery/light" : "site/"+std::to_string(group);
        const auto family=Json::array({tool,tool+"/read"}).dump();
        const auto goal=group==1 ? std::string{"巡检图书馆书架"} : group==2 ? "巡检展厅的照明设备" : "巡检展厅的资料架"+std::to_string(group);
        const auto text=group==1 ? std::string{"我在归架时会先核对索书号。"} : group==2 ? "我在布光后读回亮度设定。" : "我会核对本区标签。";
        Json cites=Json::array();
        const unsigned base=group*100;
        for (unsigned n=1;n<=16;++n) {
            Statement task(handle,"INSERT INTO task_memory(id,origin,goal,tool,body,reason,signature) VALUES(?,?,?,?, '{}','','')");
            task.bind(1,base+n); task.bind(2,std::to_string(base+n)); task.bind(3,goal); task.bind(4,tool); task.next();
            Statement obs(handle,"INSERT INTO task_observation VALUES(?,?,'off','on',?,2,1,3,'applied')");
            obs.bind(1,base+n); obs.bind(2,family); obs.bind(3,group==1 && n==16 ? "off" : "on"); obs.next();
            if (n<=2) cites.push_back(profile+"/task/"+std::to_string(base+n));
        }
        const auto version=[&](unsigned id,unsigned parent,unsigned cutoff,const std::string& summary,bool valid) {
            Json claim{{"text",summary},{"condition","any"},{"when",""},{"predict","target"},{"value",""}};
            Statement insert(handle,"INSERT INTO task_understanding VALUES(?,?,?,?,?,?,?,0,?)");
            insert.bind(1,id); insert.bind(2,family); insert.bind(3,parent); insert.bind(4,base+cutoff);
            insert.bind(5,parent ? "reinforced" : "formed"); insert.bind(6,claim.dump()); insert.bind(7,cites.dump()); insert.bind(8,valid); insert.next();
        };
        version(base,0,2,"我过去用紫色书签标记旧版本。",true);
        version(base+1,base,15,text,true);
        version(base+2,0,2,"已经撤回的金色书签。",false);
        Statement progress(handle,"INSERT INTO reflection_progress VALUES(?,?,?)");
        progress.bind(1,family); progress.bind(2,base+15); progress.bind(3,base+1); progress.next();
    }
    tx.commit();
}

// 普通主人来信走实际模型和工具入口；固定后端只提议查询，期望命中由语料独立指定。
void query(const fs::path& data,const Json& row,Measures& metrics) {
    auto db=std::make_shared<app::MessageStore>(data);
    const auto profile=db->conversation(), id=row.at("id").get<std::string>();
    std::vector<std::string> wire_hashes;
    const auto human=row.at("ask").get<std::string>();
    std::string delivered;
    Clock::time_point start;
    app::ApplicationRunConfig config;
    config.trace_directory=data/"logs";
    app::Application host({},data/"empty",data,model::Backend{[&](const model::Config& cfg,const model::Request& input)->model::Response {
        const auto wire=model::encode_request_fields(input,cfg.thinking_enabled());
        wire_hashes.push_back(platform::sha256(wire));
        metrics.max_wire=std::max(metrics.max_wire,wire.size());
        if (wire_hashes.size()==1) {
            require(wire.find(human)!=wire.npos && wire.find("我在归架时") == wire.npos,"wrong audience or unconditional injection");
            require(std::ranges::any_of(input.tools,[](const auto& t){return t.name=="memory_search" &&
                t.description.find("同义措辞")!=std::string::npos;}),"missing effective recall guidance");
            if (!row.contains("topics")) { delivered="按本轮已知情况回答。"; return {{model::Role::assistant,delivered}}; }
            auto topics=row.at("topics").get<std::vector<std::string>>();
            for (auto& term : topics) if (term.starts_with("@")) term=profile+term.substr(1);
            Json args{{"view","understanding"},{"need",row.value("need",std::string{"single"})},{"count",row.value("count",1)},{"topics",topics}};
            start=Clock::now();
            return {{model::Role::assistant,"",{{"query","memory_search",args.dump()}}},model::FinishReason::tool_calls};
        }
        metrics.roundtrips.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
        require(wire_hashes.size()==2,"unexpected extra query");
        const auto status=Json::parse(input.messages.back().content);
        const auto found=std::ranges::find_if(status.at("items"),[](const auto& s){return s.at("key")=="memory.understanding";});
        if (row.value("skip",false)) {
            require(found==status.at("items").end(),"skipped query exposed evidence");
            delivered="用本轮材料即可。";
        } else {
            require(found!=status.at("items").end(),"missing retrieval result");
            const auto& selection=found->at("value"), &items=selection.at("items");
            metrics.max_evidence=std::max(metrics.max_evidence,items.dump().size());
            require(items.dump().size()<=2048 && selection.value("ambiguous",false)==row.value("ambiguous",false),"budget or ambiguity mismatch");
            if (!row.contains("version")) {
                require(items.empty(),"stale or unrelated evidence");
                delivered=row.value("ambiguous",false) ? "你想问哪一类任务？" : "我还没有对应的解释。";
            } else {
                const auto& item=items.at(0);
                require(items.size()==1 && item.at("reference")==profile+"/understanding/"+std::to_string(row.at("version").get<unsigned>()) &&
                    item.at("standing")==row.at("standing"),"incorrect version");
                const auto& facts=item.at("facts");
                require(!facts.empty() && facts.size()<=std::min(3,row.value("count",1)),"wrong evidence count");
                if (row.at("standing")=="challenged") {
                    require(facts.at(0).at("role")=="counterexample" && facts.at(0).at("readbackRef")==profile+"/task/116/receipt/3","missed counterexample");
                    ++metrics.challenges;
                }
                delivered=item.at("claim").at("text").get<std::string>()+" 可查 "+facts.at(0).at("readbackRef").get<std::string>();
            }
        }
        return {{model::Role::assistant,delivered}};
    }},config);
    host.bind_messages(db,id,nullptr);
    host.main_messages()->terminal(id,human); drain(host);
    require(!delivered.empty() && db->page(true,0,db->watermark(),1).records.front().envelope.at("text")==delivered,"answer did not reach owner");
    std::size_t prepared=0;
    std::ifstream trace(host.trace_file(),std::ios::binary);
    for(std::string line;std::getline(trace,line);) {
        const auto entry=Json::parse(line);
        if(entry.at("kind")=="model.prepared") require(entry.at("wireSha256")==wire_hashes.at(prepared++),"different backend request");
    }
    require(prepared==wire_hashes.size() && prepared==(row.contains("topics") ? 2u : 1u),"request count mismatch");
    metrics.requests+=static_cast<unsigned>(prepared); ++metrics.cases;
}
}

// 对同一批固定问句逐档放大近邻数量，报告实收字节、反例覆盖和工具往返的分位数。
void understanding_recall(const fs::path& corpus,const fs::path& data) {
    std::ifstream in(corpus,std::ios::binary);
    std::vector<Json> rows;
    for (std::string line;std::getline(in,line);) rows.push_back(Json::parse(line));
    require(!rows.empty(),"empty retrieval corpus");
    for (const auto groups : {2u,32u,256u}) {
        const auto root=data/std::to_string(groups); fs::create_directories(root); populate(root,groups);
        Measures m;
        for (const auto& row : rows) try { query(root,row,m); }
        catch(const std::exception& e) {throw std::runtime_error(row.at("id").get<std::string>()+": "+e.what());}
        std::ranges::sort(m.roundtrips);
        std::cout<<groups*16<<" observations, "<<m.cases<<" correct, "<<m.challenges<<" challenged recalled, "
            <<m.requests<<" requests, evidence max "<<m.max_evidence<<" bytes, wire max "<<m.max_wire
            <<" bytes, tool roundtrip p95 "<<m.roundtrips.at((95*m.roundtrips.size()+99)/100-1)<<" ms\n";
    }
}
}
