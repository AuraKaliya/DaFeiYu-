#include "application.hpp"
#include "main_messages.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/model_protocol.hpp>
#include <windows.h>
#include <winsqlite/winsqlite3.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

namespace {
using namespace dfy;
using Json = nlohmann::json;
using Sources = std::map<std::string,app::StoredMessage>;

// 报告与语料编号关联的断言错误。
void require(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }

// 构造已经有获准原话的第三版档案，用正式启动路径验证原号及后续复盘。
void seed_legacy(const std::filesystem::path& data, const Json& seed, Sources& sources) {
    const auto key = seed.at("id").get<std::string>();
    {
        app::MessageStore db(data);
        const auto envelope = app::user_envelope("send",seed.at("text").get<std::string>(),{},{});
        const auto saved = db.put("terminal",key,envelope,{{"state","pending"}});
        db.update(saved.message.id,{{"state","accepted"}});
        sources.emplace(key,*db.find("terminal",key));
    }
    sqlite3* handle = nullptr;
    const auto path = (data/"main-conversation.sqlite3").u8string();
    const auto opened = sqlite3_open(reinterpret_cast<const char*>(path.c_str()),&handle);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> close(handle,&sqlite3_close);
    require(opened==SQLITE_OK && sqlite3_exec(handle,"BEGIN; DROP TABLE task_understanding; DROP TABLE reflection_progress; DROP TABLE task_observation;"
        "DROP TABLE task_memory; DROP TABLE experience_review; DROP TABLE main_experience; DROP TABLE memory_review; DROP INDEX active_memory_topic;"
        "ALTER TABLE personal_memory RENAME TO newer; CREATE TABLE personal_memory(message INTEGER PRIMARY KEY,topic TEXT NOT NULL,active INTEGER NOT NULL);"
        "INSERT INTO personal_memory SELECT message,topic,active FROM newer; DROP TABLE newer;"
        "CREATE INDEX active_memory_topic ON personal_memory(topic,message DESC) WHERE active=1; PRAGMA user_version=3; COMMIT",
        nullptr,nullptr,nullptr)==SQLITE_OK,"could not prepare v3 profile");
}

// 让业务线程推进到本回合成功结算。
void drain(app::Application& host) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (host.conversation().busy()) {
        host.advance_plugins();
        host.conversation().advance();
        require(std::chrono::steady_clock::now()<end,"dialogue timed out");
        std::this_thread::yield();
    }
    require(host.conversation().status()->result==runtime::OperationState::succeeded,"turn failed");
}

// 逐项核对选中原件、顺序、字节和来源，同时在完整送模编码中排除未选原话。
void check_request(const model::Request& request, const Sources& sources, const std::string& profile,
    const Json& row, unsigned step, bool followup) {
    const bool owner = !row.contains("audience") && !row.contains("original");
    for (const std::string name : {"memory_note","memory_search","memory_reflect","main_choose","experience_review"}) {
        const auto tool = std::ranges::find(request.tools,name,&model::ToolDefinition::name);
        require((tool!=request.tools.end())==owner,"incorrect tool audience");
    }
    const auto tail = Json::parse(request.messages.back().content);
    const auto& items = tail.at("items");
    if (row.contains("original")) {
        const auto found = std::find_if(items.begin(),items.end(),[](const auto& item){return item.at("key")=="memory.original";});
        require((found==items.end())==row.at("original").is_null(),"wrong direct original visibility");
        if (found!=items.end()) {
            const auto& source = sources.at(row.at("original").get<std::string>());
            require(found->at("value").at("quote")==source.envelope.at("text") &&
                found->at("value").at("reference")==profile+"/"+std::to_string(source.id),"wrong direct original");
        }
    }
    const auto batch = std::find_if(items.begin(),items.end(),[](const auto& item){return item.at("key")=="memory.matches";});
    const bool searched = !followup && step>=row.value("searchStep",1u) && row.contains("hits");
    require((batch!=items.end())==searched,"unexpected search projection");
    const auto expected = searched ? row.at("hits").get<std::vector<std::string>>() : std::vector<std::string>{};
    if (searched) {
        const auto& selection = batch->at("value");
        if (row.contains("queryDue")) require(selection.value("reviewDue",std::string{})==row.at("queryDue").get<std::string>(),
            "wrong review trigger on recall");
        const auto& hits = selection.at("items");
        require(hits.size()==expected.size() && hits.size()<=3 && hits.dump().size()<=2048 &&
            selection.at("limited")==row.at("limited"),"selection size or limit differs");
        for (std::size_t i=0;i<hits.size();++i) {
            const auto& source = sources.at(expected[i]);
            require(hits[i].at("reference")==profile+"/"+std::to_string(source.id) && hits[i].at("source")==source.source &&
                hits[i].at("utcMs")==source.envelope.value("sourceUtcMs",source.utc_ms) &&
                hits[i].at("quote")==source.envelope.at("text"),"different evidence or order");
            if (row.contains("reviewed")) {
                const auto& gold = row.at("reviewed").at(i);
                if (gold.is_null()) require(!hits[i].contains("standing"),"raw evidence acquired a conclusion");
                else {
                    require(hits[i].at("standing")==gold.at("standing") && hits[i].at("mentions")==gold.at("mentions") &&
                        hits[i].at("conclusion")==gold.at("conclusion") && hits[i].at("latestReference")==profile+"/"+
                        std::to_string(sources.at(gold.at("latest").get<std::string>()).id),"wrong standing, summary or support");
                    const auto previous = gold.contains("previous") ? profile+"/"+
                        std::to_string(sources.at(gold.at("previous").get<std::string>()).id) : std::string{};
                    require(hits[i].value("previousReference",std::string{})==previous,"wrong or revoked predecessor");
                }
            }
        }
    }
    const auto wire = model::encode_request_fields(request,false);
    for (const auto& [key,source] : sources) {
        if (key==row.at("id").get<std::string>()) continue;
        const auto& quote = source.envelope.at("text").get_ref<const std::string&>();
        const bool selected = std::ranges::find(expected,key)!=expected.end() || (row.contains("original") && row.at("original")==key);
        require((wire.find(quote)!=std::string::npos)==selected,"wrong full-request visibility for "+key);
    }
}

// 固定后端从实际送入的复盘材料逐项引用，另以语料给出的依据检查最终投递。
std::string cited_answer(const Json& items) {
    std::string reply;
    for (const auto& item : items) {
        reply += item.at("standing")=="current" ? "现在采用：" : "当时采用：";
        reply += item.at("conclusion").get<std::string>()+"。有"+std::to_string(item.at("mentions").get<unsigned>())+
            "条原话为据。原话："+item.at("quote").get<std::string>()+
            " ["+item.at("reference").get<std::string>()+"]\n";
    }
    return reply;
}

// 由实际送模的材料生成固定答复，逐份引用当场实选与投递号。
std::string experience_answer(const Json& value) {
    std::string answer = value.at("kind")=="why" ? "现在我猜："+value.at("interpretation").get<std::string>() : "同样条件下，我的实选依次是：";
    for (const auto& event : value.at("items")) {
        answer += "\n"+event.at("scene").get<std::string>()+"，"+event.at("choice").get<std::string>()+" ["+event.at("reference").get<std::string>()+"]";
        if (event.contains("impression") && !event.at("impression").get_ref<const std::string&>().empty())
            answer += " 当时我说："+event.at("impression").get<std::string>();
    }
    return answer;
}

// 按语料的独立期望核对每步实际装入的原场次，连同引用的来信和当时送达的原话一起核实。
void check_experiences(const model::Request& request, const Sources& episodes, const std::string& profile,
    const Json& row, unsigned step, bool followup) {
    const auto status = Json::parse(request.messages.back().content);
    const auto& items = status.at("items");
    const auto batch = std::ranges::find_if(items,[](const Json& item){return item.at("key")=="memory.experiences";});
    const bool read = !followup && row.contains("events") && step>=row.value("searchStep",1u);
    require((batch!=items.end())==read,"incorrect experience visibility");
    if (read) {
        const auto& value = batch->at("value");
        const auto& evidence = value.at("items");
        require(evidence.size()==row.at("events").size() && evidence.size()<=3 && evidence.dump().size()<=2048 &&
            value.at("limited")==row.value("limited",false),"wrong experience budget");
        require(value.value("kind",std::string{})==row.value("kind",std::string{}) &&
            value.value("interpretation",std::string{})==row.value("why",std::string{}),"changed review or axis");
        for (std::size_t i=0;i<evidence.size();++i) {
            const auto& saved = episodes.at(row.at("events")[i].get<std::string>());
            const auto& event = evidence.at(i);
            require(event.at("reference")==profile+"/"+std::to_string(saved.id) &&
                event.at("parentReference")==profile+"/"+saved.envelope.at("replyTo").get<std::string>() &&
                event.at("utcMs")==saved.utc_ms && event.at("deliveredText")==saved.envelope.at("text"),"wrong delivery or original input");
            for (const auto& [key,field] : saved.envelope.at("experience").items())
                require(event.at(key)==field,"rewrote contemporaneous observation");
        }
    }
    const auto wire = model::encode_request_fields(request,false);
    for (const auto& forbidden : row.value("absent",std::vector<std::string>{}))
        require(wire.find(forbidden)==wire.npos,"private or unsupported conclusion in backend request");
}

// 从同一档案重开主聊天，播放固定的模型提议，并独立核对实际工具回执与送模材料。
// 步骤：绑定来源与受众 → 按正常入口交付 → 验证每步回执、下一轮及 Trace 摘要。
std::size_t run(const Json& row, const std::filesystem::path& data, Sources& sources, Sources& episodes) {
    const auto id = row.at("id").get<std::string>();
    auto text = row.at("text").get<std::string>()+std::string(row.value("padding",0u),'A');
    const auto steps = row.value("steps",Json::array());
    const auto states = row.value("states",Json::array());
    const auto db = std::make_shared<app::MessageStore>(data);
    // 语料用稳定名称引用之前的原话，调用现场代入该档案实际签发的消息号。
    const auto reference = [&](std::string key) {
        if (!key.starts_with("$")) return key;
        key.erase(0,1);
        if (key.starts_with("#")) {
            if (key.substr(1)==id && !episodes.contains(id)) {
                for (const auto& msg : db->page(true,0,db->watermark(),3).records)
                    if (msg.source=="main" && msg.envelope.contains("experience") && msg.envelope.at("instance")==id)
                        return db->conversation()+"/"+std::to_string(msg.id);
            }
            return db->conversation()+"/"+std::to_string(episodes.at(key.substr(1)).id);
        }
        if (sources.contains(key)) return db->conversation()+"/"+std::to_string(sources.at(key).id);
        return db->conversation()+"/"+std::to_string(db->find("terminal",key).value().id);
    };
    if (text.starts_with("回想 $")) text = "回想 "+reference(text.substr(std::string_view("回想 ").size()));
    app::ApplicationRunConfig config;
    config.trace_directory = data/"traces"/id;
    bool followup = false, room_check = false;
    std::string previous_snapshot;
    std::vector<model::Request> requests;
    app::Application host({},data/"empty",data,model::Backend{[&](const auto&,const model::Request& request)->model::Response {
        requests.push_back(request);
        const auto step = Json::parse(request.messages.back().content).at("step").get<unsigned>()-1;
        if (room_check) return {{model::Role::assistant,"收到。"}};
        if (followup && !step)
            return {{model::Role::assistant,"",{{"probe","status_read",Json{{"snapshot",previous_snapshot}}.dump()}}},model::FinishReason::tool_calls};
        if (!followup && step<steps.size()) {
            const auto action = steps.at(step).begin();
            auto args = action.value();
            if (action.key()=="memory_reflect") for (const auto field : {"prior","later"})
                args[field] = reference(args.at(field).get<std::string>());
            if (action.key()=="memory_search") for (auto& term : args.at("topics")) term = reference(term.get<std::string>());
            if (action.key()=="experience_review") for (auto& ref : args.at("evidence")) ref = reference(ref.get<std::string>());
            return {{model::Role::assistant,"",{{"call"+std::to_string(step),action.key(),args.dump()}}},model::FinishReason::tool_calls};
        }
        if (row.value("citeExperience",false)) {
            const auto status = Json::parse(request.messages.back().content);
            for (const auto& item : status.at("items")) if (item.at("key")=="memory.experiences")
                return {{model::Role::assistant,experience_answer(item.at("value"))}};
        }
        if (row.value("cite",false)) {
            const auto status = Json::parse(request.messages.back().content);
            for (const auto& item : status.at("items")) if (item.at("key")=="memory.matches")
                return {{model::Role::assistant,cited_answer(item.at("value").at("items"))}};
        }
        return {{model::Role::assistant,"收到。"}};
    }},config);
    host.bind_messages(db,id,nullptr);
    auto& channel = *host.main_messages();
    if (row.contains("revoke")) channel.change_memory(reference("$"+row.at("revoke").get<std::string>()),std::nullopt);
    const auto audience = row.value("audience",std::string{"owner"});
    if (audience!="owner") host.post_room_event(app::RoomEvent::addressed,"书房",audience=="room" ? text : "待收的来信");
    if (audience=="room") require(host.review_room()==app::MailboxIntakeResult::submitted,"room not admitted");
    else if (row.contains("utcMs")) {
        const auto envelope = app::user_envelope("send",text,{},{},row.at("utcMs").get<std::int64_t>());
        const auto saved = db->put("desktop",id,envelope,{{"state","pending"}});
        require(channel.deliver(saved.message.id,envelope,saved.message.utc_ms).at("state")=="accepted","desktop original rejected");
    } else channel.terminal(id,text);
    drain(host);
    if (row.contains("chosen")) {
        const auto& args = row.at("steps").at(0).at("main_choose");
        const auto parent = db->find(row.contains("utcMs")?"desktop":"terminal",id).value();
        const auto page = db->page(true,0,db->watermark(),4);
        unsigned found = 0;
        for (const auto& msg : page.records) {
            if (msg.source!="main" || msg.envelope.value("replyTo",std::string{})!=std::to_string(parent.id) || !msg.envelope.contains("experience")) continue;
            require(msg.receipt.at("state")=="delivered" && msg.envelope.at("experience")==args &&
                msg.envelope.at("text")=="我选「"+args.at("choice").get<std::string>()+"」。"+args.at("impression").get<std::string>(),
                "choice not actually delivered");
            episodes.emplace(id,msg);
            ++found;
        }
        require(found==(row.at("chosen")==true ? 1u : 0u),"wrong number of delivered decisions");
    }
    if (!steps.empty() && steps.front().contains("memory_note")) sources.emplace(id,*db->find(row.contains("utcMs")?"desktop":"terminal",id));
    require(requests.size()==steps.size()+1 && states.size()==steps.size(),"wrong number of decisions");
    for (unsigned i=0;i<requests.size();++i) {
        check_request(requests[i],sources,db->conversation(),row,i,false);
        check_experiences(requests[i],episodes,db->conversation(),row,i,false);
        if (!i) continue;
        const auto& messages = requests[i].messages;
        const auto& receipt = messages.at(messages.size()-2);
        require(receipt.role==model::Role::tool && receipt.tool_call_id=="call"+std::to_string(i-1),"unpaired tool receipt");
        const auto result = Json::parse(receipt.content.substr(receipt.content.find('{')));
        require(result.at("state")==states.at(i-1),"unexpected receipt: "+result.dump());
        if (result.at("state")=="delivered" && steps.at(i-1).contains("main_choose"))
            require(result.at("reference")==reference("$#"+id) && result.contains("reviewDue")==row.value("dueChoice",false),"wrong experience identity or trigger");
        if (result.at("state")=="saved") require(result.at("reference")==db->conversation()+"/"+std::to_string(sources.at(id).id),"wrong stored original");
        if (result.at("state")=="saved" && row.contains("due"))
            require(result.value("reviewDue",std::string{})==row.at("due").get<std::string>(),"wrong review trigger");
    }
    if (row.value("citeExperience",false)) {
        const auto msg = db->page(true,0,db->watermark(),1).records.at(0);
        const auto& answer = msg.envelope.at("text").get_ref<const std::string&>();
        require(msg.source=="main" && msg.receipt.at("state")=="delivered" &&
            (row.at("kind")=="why" ? answer.starts_with("现在我猜："+row.at("why").get<std::string>()) : answer.starts_with("同样条件下，我的实选依次是：")),
            "missing first-person review at delivery");
        for (const auto& key : row.at("events")) {
            const auto& saved = episodes.at(key.get<std::string>());
            const auto& args = saved.envelope.at("experience");
            require(answer.find(args.at("scene").get<std::string>()+"，"+args.at("choice").get<std::string>()+" ["+
                reference("$#"+key.get<std::string>())+"]")!=answer.npos,"delivered answer lacks cited experience");
            if (!args.at("impression").get_ref<const std::string&>().empty())
                require(answer.find("当时我说："+args.at("impression").get<std::string>())!=answer.npos,"lost contemporaneous impression");
        }
    }
    if (row.value("cite",false)) {
        const auto message = db->page(true,0,db->watermark(),1).records.at(0);
        require(message.source=="main" && message.receipt.at("state")=="delivered","cited answer not delivered");
        const auto& answer = message.envelope.at("text").get_ref<const std::string&>();
        for (std::size_t i=0;i<row.at("hits").size();++i) {
            const auto& source = sources.at(row.at("hits").at(i).get<std::string>());
            const auto& gold = row.at("reviewed").at(i);
            const std::string label = gold.at("standing")=="current" ? "现在采用：" : "当时采用：";
            require(answer.find(label+gold.at("conclusion").get<std::string>()+"。有"+
                std::to_string(gold.at("mentions").get<unsigned>())+"条原话为据")!=answer.npos &&
                answer.find(source.envelope.at("text").get<std::string>()+" ["+db->conversation()+"/"+std::to_string(source.id)+"]")!=answer.npos,
                "delivered answer lost standing or original citation");
        }
    }
    if (row.contains("followup")) {
        previous_snapshot = Json::parse(requests.back().messages.back().content).at("snapshot");
        followup = true;
        channel.terminal(id+"-next",row.at("followup").get<std::string>());
        drain(host);
        require(requests.size()==steps.size()+3,"missing snapshot probe");
        check_request(requests.at(requests.size()-2),sources,db->conversation(),row,0,true);
        check_request(requests.back(),sources,db->conversation(),row,1,true);
        check_experiences(requests.back(),episodes,db->conversation(),row,1,true);
        const auto& content = requests.back().messages.at(requests.back().messages.size()-2).content;
        const auto read = Json::parse(content.substr(content.find('{')));
        require(read.at("state")=="ready" && read.at("status").at("omittedDecisionOnly")==1,"historic snapshot bypassed search budget");
    }
    if (row.contains("roomAfter")) {
        room_check = true;
        const auto before = requests.size();
        host.post_room_event(app::RoomEvent::addressed,"书房","有人进了书房。");
        require(host.review_room()==app::MailboxIntakeResult::submitted,"private-to-room handover failed");
        drain(host);
        require(requests.size()==before+1,"unexpected room decisions");
        Json room{{"id",id},{"audience","room"},{"absent",row.at("roomAfter")}};
        check_request(requests.back(),sources,db->conversation(),room,0,false);
        check_experiences(requests.back(),episodes,db->conversation(),room,0,false);
    }
    std::ifstream trace(host.trace_file(),std::ios::binary);
    std::size_t verified = 0;
    for (std::string line;std::getline(trace,line);) {
        const auto event = Json::parse(line);
        if (event.at("kind")!="model.prepared") continue;
        const auto wire = model::encode_request_fields(requests.at(verified++),event.at("thinking").get<bool>());
        require(platform::sha256(wire)==event.at("wireSha256").get<std::string>(),"prepared digest differs from backend request");
    }
    require(verified==requests.size(),"missing prepared request");
    return verified;
}
}

// 外置语料保留输入、模型提议与独立的证据答案，整套测试无需网络或模型服务。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=3) return 2;
        const auto data = std::filesystem::path(argv[2])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        std::ifstream corpus(std::filesystem::path(argv[1]),std::ios::binary);
        Sources sources, episodes;
        unsigned cases = 0;
        std::size_t requests = 0;
        for (std::string line;std::getline(corpus,line);) {
            const auto row = Json::parse(line);
            if (!cases && row.contains("legacy")) seed_legacy(data,row.at("legacy"),sources);
            try { requests += run(row,data,sources,episodes); }
            catch (const std::exception& e) { throw std::runtime_error(row.at("id").get<std::string>()+": "+e.what()); }
            ++cases;
        }
        require(cases>0,"empty dialogue corpus");
        std::cout << cases << " dialogue cases, " << requests << " verified backend requests\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
