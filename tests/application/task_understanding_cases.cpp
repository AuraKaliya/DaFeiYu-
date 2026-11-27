#include "task_memory_fixture.hpp"
#include "main_messages.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/model_protocol.hpp>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <winsqlite/winsqlite3.h>

namespace dfy::tests::task_memory {
namespace {
// 从已实写的 Trace 查一次空闲推断的结算结果。
std::string outcome(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);
    for (std::string line;std::getline(in,line);) {
        const auto entry=Json::parse(line);
        if (entry.at("kind")=="memory.reflection_settled") return entry.at("outcome");
    }
    return {};
}
struct Evidence {
    std::map<std::string,std::vector<Json>> works;
    std::map<std::string,std::string> refs;
    std::size_t foreground=0, idle=0, tasks=0;
};
// 逐项与插件实际执行 Trace 对读，含准备初值的任务只取目标任务的三份原回执。
void verify_fact(const Json& fact, Evidence& evidence, const app::MessageStore& store) {
    const auto id=fact.at("goal").get<std::string>();
    const auto& works=evidence.works.at(id);
    require(works.size()>=3,"plan-only or unverified evidence");
    const auto& before=works[works.size()-3], &action=works[works.size()-2], &after=works.back();
    const auto base=fact.at("taskRef").get<std::string>();
    const auto receipt=[&](const Json& w){return base+"/receipt/"+std::to_string(w.at("candidate").get<std::uint64_t>());};
    require(base.starts_with(store.conversation()+"/task/") && fact.at("actionRef")==base+"/action/"+std::to_string(action.at("candidate").get<std::uint64_t>()) &&
        fact.at("receiptRef")==receipt(action) && fact.at("baselineRef")==receipt(before) && fact.at("readbackRef")==receipt(after) &&
        fact.at("before")==before.at("output") && fact.at("actual")==after.at("output") && fact.at("effect")==action.at("effect"),"fact detached from original receipts");
    const auto [it,inserted]=evidence.refs.emplace(id,base);
    require(inserted || it->second==base,"task identity changed on restart");
}

// 一步语料重新绑定同一档案。固定模型在实际收到的原件上提议解释，私聊用另一份请求读取结果。
void exercise(const Json& row, const fs::path& data, Evidence& evidence, unsigned serial) {
    auto db=std::make_shared<app::MessageStore>(data);
    auto now=agent::ToolTime{};
    app::ApplicationRunConfig config;
    config.tool_clock=[&]{return now;}; config.trace_directory=data/"logs";
    std::mutex mutex;
    std::map<std::string,unsigned> wires;
    std::atomic<bool> entered=false, release=false;
    Json selected;
    bool first=true, room=false, room_received=false;
    const auto lookup=row.value("selector",std::string{"example.status/set_focus"});
    const auto term=evidence.refs.contains(lookup) ? evidence.refs.at(lookup) : lookup;
    std::string delivered;
    std::string original_ref;
    unsigned query_number=0;
    app::Application host({},data/"empty",data,model::Backend{[&](const model::Config& cfg,const model::Request& request)->model::Response {
        {
            std::lock_guard lock(mutex);
            ++wires[platform::sha256(model::encode_request_fields(request,cfg.thinking_enabled()))];
        }
        const auto tail=Json::parse(request.messages.back().content);
        if (tail.contains("observations")) {
            require(request.tools.empty() && request.messages.size()==2 &&
                request.messages[0].content.find("大肥鱼")!=std::string::npos,"unexpected idle authority or persona");
            std::set<std::string> observed;
            Json cites=Json::array();
            for (const auto& fact : tail.at("observations")) {
                const auto& goal=fact.at("goal").get_ref<const std::string&>();
                observed.insert(goal);
                cites.push_back(fact.at("taskRef"));
            }
            require(observed==row.at("inputs").get<std::set<std::string>>() && cites.size()==observed.size(),"wrong original tasks selected");
            const auto& previous=tail.at("previousUnderstanding");
            require((previous.is_null() ? std::string{} : previous.at("text").get<std::string>())==row.value("prior",std::string{}),"wrong previous interpretation");
            entered=true;
            if (row.contains("interrupt")) {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while (!release && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
            }
            if (row.value("forge",false)) cites.back()="foreign/task/123";
            if (!row.contains("claim")) return {{model::Role::assistant,R"({"defer":true})"}};
            return {{model::Role::assistant,Json{{"claim",row.at("claim")},{"cites",cites}}.dump()}};
        }
        if (room) {
            require(model::encode_request_fields(request,false).find(row.at("text").get<std::string>())==std::string::npos,
                "private interpretation appeared in room");
            require(std::ranges::none_of(request.tools,[](const auto& tool){return tool.name=="memory_search";}),"room received private recall tool");
            room_received=true;
            return {{model::Role::assistant,"收到。"}};
        }
        if (first) {
            first=false;
            Json args{{"topics",{term}},{"need","multiple"},{"count",row.value("count",3)},{"offset",row.value("offset",0)},{"view","understanding"}};
            if (!original_ref.empty()) args={{"topics",{original_ref}},{"need","single"},{"count",1},{"view","tasks"}};
            return {{model::Role::assistant,"",{{"read","memory_search",args.dump()}}},model::FinishReason::tool_calls};
        }
        if (!original_ref.empty()) for (const auto& item : tail.at("items")) if (item.at("key")=="memory.tasks") {
            selected=item.at("value").at("items");
            delivered=selected.at(0).at("payload").get<std::string>();
            return {{model::Role::assistant,delivered}};
        }
        for (const auto& item : tail.at("items")) if (item.at("key")=="memory.understanding") {
            selected=item.at("value").at("items");
            require(selected.dump().size()<=2048,"recall budget exceeded");
            delivered="还没有可引用的理解。";
            if (!selected.empty()) {
                const auto& v=selected.front();
                delivered="我的理解是「"+v.at("claim").at("text").get<std::string>()+"」。版本状态 "+v.at("standing").get<std::string>();
                for (const auto& f : v.at("facts")) delivered+="。实际读回「"+f.at("actual").get<std::string>()+"」，出处 "+f.at("readbackRef").get<std::string>();
            }
            return {{model::Role::assistant,delivered}};
        }
        throw std::runtime_error("missing interpretation projection");
    }},config);
    host.bind_messages(db,"u"+std::to_string(serial),nullptr);
    const auto query=[&] {
        first=true;
        host.main_messages()->terminal("q"+std::to_string(serial)+"-"+std::to_string(++query_number),
            original_ref.empty() ? "你现在怎样理解这几次经历？" : "请展开刚才提到的原件 "+original_ref);
        drain(host);
        require(selected.is_array() && db->page(true,0,db->watermark(),1).records.front().envelope.at("text")==delivered,"missing delivered citations");
    };
    if (row.at("op")=="reflect") {
        for (unsigned tick=0;tick<5;++tick) {
            host.advance_plugins(); now+=std::chrono::seconds(1);
            require(!entered,"idle request bypassed quiet window");
        }
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (!entered) {
            host.advance_plugins(); now+=std::chrono::seconds(1);
            require(std::chrono::steady_clock::now()<deadline,"idle request missing");
            std::this_thread::yield();
        }
        if (row.contains("interrupt")) {
            if (row.at("interrupt")=="foreground") query();
            else host.main_messages()->change_memory(evidence.refs.at(row.at("interrupt").get<std::string>()),std::nullopt);
            release=true;
        }
        std::string settled;
        do {
            host.advance_plugins();
            settled=outcome(host.trace_file());
            require(std::chrono::steady_clock::now()<deadline,"idle response never settled");
        } while (settled.empty());
        require(settled==row.value("outcome",std::string{"accepted"}),"unexpected proposal outcome "+settled);
    } else if (row.at("op")=="revoke") {
        host.main_messages()->change_memory(evidence.refs.at(row.at("task").get<std::string>()),std::nullopt);
    } else {
        query();
        if (row.value("missing",false)) require(selected.empty(),"revoked, unrelated or stale version surfaced");
        else {
            const auto& v=selected.at(0);
            require(v.at("author")=="main" && v.at("kind")=="interpretation" && v.at("claim").at("text")==row.at("text") &&
                v.at("relation")==row.at("relation") && v.at("standing")==row.at("standing") &&
                v.at("supportCount")==row.at("supports") && v.at("counterexampleCount")==row.value("contrary",0),"wrong interpretation, state or counts");
            if (row.contains("alias")) evidence.refs[row.at("alias").get<std::string>()]=v.at("reference");
            if (row.contains("parent")) require(v.at("previousReference")==evidence.refs.at(row.at("parent").get<std::string>()),"different parent");
            if (row.contains("contrast")) require(v.at("revisedFrom")==evidence.refs.at(row.at("contrast").get<std::string>()),"lost revision basis");
            if (row.contains("next")) require(v.at("nextOffset")==row.at("next"),"wrong continuation position");
            std::vector<std::string> actual;
            for (const auto& f : v.at("facts")) {
                verify_fact(f,evidence,*db);
                if (row.contains("cited")) require(f.at("cited")==row.at("cited").at(actual.size()),"confused cited and observed evidence");
                actual.push_back(f.at("goal").get<std::string>()+"/"+f.at("role").get<std::string>()+(f.at("challengedPrevious")==true ? "/prior" : ""));
            }
            require(actual==row.at("facts").get<std::vector<std::string>>(),"different supporting or challenging originals: "+Json(actual).dump());
            if (row.value("expand",false)) {
                const auto f=v.at("facts").front();
                const auto& w=evidence.works.at(f.at("goal").get<std::string>());
                for (const auto& [key,payload] : std::vector<std::pair<std::string,Json>>{
                    {"actionRef",w[w.size()-2].at("arguments")},{"receiptRef",w[w.size()-2].at("output")},
                    {"baselineRef",w[w.size()-3].at("output")},{"readbackRef",w.back().at("output")}}) {
                    original_ref=f.at(key); query();
                    require(selected.at(0).at("reference")==original_ref && delivered==payload.get<std::string>(),"reference did not open its original");
                }
            }
            if (row.value("room",false)) {
                room=true; host.post_room_event(app::RoomEvent::addressed,"书房","有人到了。"); host.advance_plugins(); drain(host);
                require(room_received,"room case did not reach backend");
            }
        }
    }
    std::ifstream in(host.trace_file(),std::ios::binary);
    for (std::string line;std::getline(in,line);) {
        const auto entry=Json::parse(line);
        const bool idle=entry.at("kind")=="memory.reflection_prepared";
        if (!idle && entry.at("kind")!="model.prepared") continue;
        const auto key=entry.at("wireSha256").get<std::string>();
        require(wires.contains(key) && wires[key],"trace differs from real backend request");
        --wires[key];
        if (idle) ++evidence.idle; else ++evidence.foreground;
    }
    for (const auto& [key,count] : wires) require(!count,"request missing from trace");
}
}

// 在同一档案跨启动执行通用场景，真实工具原件和异步推断使用独立的验收答案。
void understanding_cases(const fs::path& file,const fs::path& data,const fs::path& panel,const fs::path& counter) {
    std::ifstream in(file,std::ios::binary);
    Evidence evidence;
    unsigned serial=0;
    for (std::string line;std::getline(in,line);) {
        const auto row=Json::parse(line); ++serial;
        try {
            if (row.at("op")=="task") {
                const auto id=row.at("id").get<std::string>();
                const bool count=row.value("counter",false);
                auto task=row;
                task["tool"]=count ? "test.tools/increment" : "example.status/set_focus";
                task["script"]=row.contains("expected") ? Json::array({"plan","read","change","read","claim"}) : Json::array({"change","claim"});
                evidence.works[id]=perform(task,data,count ? counter : panel,id,evidence.tasks);
            } else if (row.at("op")=="legacy") {
                sqlite3* db=nullptr;
                const auto path=(data/"main-conversation.sqlite3").u8string();
                require(sqlite3_open(reinterpret_cast<const char*>(path.c_str()),&db)==SQLITE_OK,"fixture database unavailable");
                const auto rc=sqlite3_exec(db,"BEGIN; DROP TABLE task_understanding; DROP TABLE reflection_progress; DROP TABLE task_observation; PRAGMA user_version=6; COMMIT",nullptr,nullptr,nullptr);
                sqlite3_close(db); require(rc==SQLITE_OK,"could not prepare v6 profile");
            } else exercise(row,data,evidence,serial);
        } catch (const std::exception& e) { throw std::runtime_error("case "+std::to_string(serial)+": "+e.what()); }
    }
    require(serial>0 && evidence.idle>0,"missing understanding corpus");
    std::cout<<serial<<" cases, "<<evidence.tasks<<" task requests, "<<evidence.idle<<" idle requests, "<<evidence.foreground<<" foreground requests\n";
}
}
