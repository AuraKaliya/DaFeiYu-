#include "application.hpp"
#include "application_commands.hpp"
#include "main_messages.hpp"
#include "knowledge_probe.hpp"
#include "archive_fixture.hpp"
#include <dafeiyu/agent_abi_v2.h>
#include <dafeiyu/digest.hpp>
#include <dafeiyu/host_services.hpp>
#include <dafeiyu/model_protocol.hpp>
#include <dafeiyu/tool_abi_v2.h>
#include <atomic>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace {
using namespace dfy;
using Json=nlohmann::json;
namespace fs=std::filesystem;
// 用稳定的场景号报告首个违约。
void check(bool ok,const std::string& why) { if (!ok) throw std::runtime_error(why); }
// 将仍在作用域内的 UTF-8 文本借给一次 C ABI 调用。
dfy_v2_string_view view(std::string_view text) { return {text.data(),text.size()}; }
// 在真实加载代次的绑定中协商不可变接口。
template<class Api> const Api& service(std::string_view name,unsigned version) {
    const void* table=nullptr;
    check(plugins::HostServices::query(view(name),version,&table,nullptr)==DFY_V2_OK && table,"service unavailable");
    return *static_cast<const Api*>(table);
}
struct Fixture {
    fs::path root,panel,counter;
    std::map<std::string,std::uint64_t> sessions,originals;
    std::set<std::string> markers;
    std::string source,signature;
    std::vector<std::string> hashes;
    Json expected;
    Json child_turn;
    Json mail_case;
    tests::KnowledgeProbe knowledge;
    Json expected_knowledge;
    std::map<std::string,std::uint64_t> letters;
    std::map<std::string,std::string> bodies;
    std::optional<bool> expect_persona;
    std::string failure,reply;
    unsigned mail_step=0,mail_reads=0,mail_received=0,kb_reads=0;
    std::atomic<bool> entered=false,release=false;
    std::mutex wire_mutex;
    bool holding=false;
    unsigned hosts=0,received=0;
    std::size_t task_requests=0;
    std::shared_ptr<app::MessageStore> db;
    std::unique_ptr<app::Application> host;
    agent::ToolTime now{};
    Json idle_case;
    unsigned window_tokens=0;
    std::size_t pending_idle=0;
    std::string archive="main";
    std::map<std::string,std::string> profile_ids;
    // 测试中的第二份独立档案与第一份使用同一插件安装目录。
    fs::path data_dir() const {return archive=="main" ? root : root/"second-profile";}
    // 从实际 Trace 中筛出跨工具复盘已结算的结果。
    std::vector<std::string> idle_outcomes(const std::string& scope) const {
        std::ifstream in(host->trace_file(),std::ios::binary); std::vector<std::string> found;
        for(std::string line;std::getline(in,line);) {
            const auto e=Json::parse(line);
            if(scope=="prepared" && e.at("kind")=="memory.reflection_prepared") found.push_back(e.at("wireSha256"));
            if(e.at("kind")!="memory.reflection_settled") continue;
            const auto recorded=e.value("scope",std::string{});
            if(scope=="all") found.push_back(recorded+"/"+e.at("outcome").get<std::string>());
            else if(recorded==scope) found.push_back(e.at("outcome"));
        }
        return found;
    }

    // 重建应用、数据库连接和插件实例，保留原档案与已安装的插件包。
    void restart(const Json& settings=Json::object()) {
        if (host) audit();
        host.reset(); db.reset(); archive=settings.value("archive",archive);
        check(archive=="main" || archive=="secondary","unknown test archive");
        if(settings.contains("legacy")) tests::legacy_archive(data_dir(),settings.at("legacy"));
        if(settings.contains("legacyLetter")) tests::legacy_letter_scope(data_dir(),knowledge.resolve(settings.at("legacyLetter")),knowledge.resolve(settings.at("newBasis")));
        db=std::make_shared<app::MessageStore>(data_dir());
        app::ApplicationRunConfig config; config.trace_directory=root/"logs"; config.tool_clock=[this]{return now;};
        model::Config mc; mc.context_window_tokens=window_tokens; if(window_tokens) mc.reserved_output_tokens=128;
        host=std::make_unique<app::Application>(mc,root/"packages",data_dir(),
            model::Backend{[this](const model::Config& cfg,const model::Request& request){
                try {return answer(cfg,request);} catch(const std::exception& e){
                    if (!idle_case.is_object() || !idle_case.value("transport",false)) failure=e.what();
                    throw;
                }
            }},config);
        host->bind_messages(db,"child-"+std::to_string(++hosts),nullptr);
        if (hosts==1) { host->plugins().install(panel); host->plugins().install(counter); }
        for (const auto& plugin : settings.value("plugins",std::vector<std::string>{"example.status","test.tools"}))
            check(host->plugins().load(plugin)==0,"plugin not loaded");
        hashes.clear();
    }
    // 核查实际请求、受众和来源；保留一次可控的迟到答复供在途撤权验收。
    model::Response answer(const model::Config& cfg,const model::Request& request) {
        const auto wire=model::encode_request_fields(request,cfg.thinking_enabled());
        {std::lock_guard lock(wire_mutex); hashes.push_back(platform::sha256(wire)); ++received;}
        const auto tail=Json::parse(request.messages.back().content,nullptr,false);
        if (tail.is_object() && tail.contains("receivedLetter")) {
            if(idle_case.is_object() && idle_case.value("scope",std::string{"letter_digest"})!="letter_digest") return {{model::Role::assistant,R"({"defer":true})"}};
            check(idle_case.is_object() && request.tools.empty() && tail.at("receivedLetter")==letter_ref(idle_case.at("ref")) &&
                request.messages.size()==2 && tail.at("sourceAuthor").get<std::string>().find("/child/")!=std::string::npos,"wrong private digest source");
            const auto& spans=tail.at("passages"); const auto& raw=bodies.at(idle_case.at("ref").get<std::string>().substr(1));
            check(spans.size()<=64,"too many digest passages");
            for(const auto& span:spans) {const auto q=span.at("text").get<std::string>();
                check(raw.substr(span.at("offset"),q.size())==q,"editor received fabricated excerpt");}
            if (idle_case.value("transport",false)) throw std::runtime_error("fixture transport closed");
            Json picks=Json::array();
            for(const auto& pick:idle_case.value("picks",Json::array())) {
                auto it=std::ranges::find_if(spans,[&](const auto& s){return s.at("text").template get<std::string>().find(pick.at("contains").template get<std::string>())!=std::string::npos;});
                check(it!=spans.end(),"late report detail missing from private backend");
                picks.push_back({{"role",pick.at("role")},{"offset",it->at("offset")}});
            }
            entered=true;
            if(idle_case.value("hold",false)) {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6);
                while(!release && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
            }
            return {{model::Role::assistant,idle_case.contains("propose") ? idle_case.at("propose").dump() : Json{{"picks",picks}}.dump()}};
        }
        if (tail.is_object() && tail.contains("observations")) return {{model::Role::assistant,R"({"defer":true})"}};
        if (tail.is_object() && tail.contains("caseFacts")) {
            if(idle_case.is_object() && idle_case.value("scope",std::string{"cross_tool"})!="cross_tool") return {{model::Role::assistant,R"({"defer":true})"}};
            check(idle_case.is_object() && request.tools.empty() && tail.at("case")==idle_case.at("case") &&
                tail.at("evidenceState")==idle_case.at("state"),"unscheduled or incorrect cross-tool review");
            Json refs=Json::array();
            for(const auto& f : tail.at("caseFacts")) if(f.at("origin")=="main_readback") refs.push_back(f.at("reference"));
            if(idle_case.contains("cites")) {refs=Json::array(); for(const auto& ref:idle_case.at("cites")) refs.push_back(knowledge.resolve(ref));}
            check(tail.at("previousUnderstanding").is_object()==idle_case.value("prior",false),"wrong prior interpretation at idle");
            const auto decision=idle_case.value("judgement",tail.at("evidenceState")=="conflicting_observations" ? std::string{"unresolved"} : tail.at("evidenceState").get<std::string>());
            if(idle_case.value("foreground",false)) {
                entered=true; const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6);
                while(!release && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
            }
            return {{model::Role::assistant,Json{{"text",idle_case.at("text")},{"judgement",decision},{"cites",refs}}.dump()}};
        }
        if(idle_case.is_object() && idle_case.value("foreground",false)) {
            check(wire.find("先回应眼前的交接")!=wire.npos && wire.find("caseFacts")==wire.npos,"private batch in foreground");
            return {{model::Role::assistant,"眼前的交接我先听你说。"}};
        }
        if (knowledge.row.is_object()) return knowledge.answer(request);
        if (mail_case.is_object()) return read_mail(request);
        if (expect_persona) check((request.messages.front().content.find("你叫大肥鱼")!=std::string::npos)==*expect_persona,"wrong startup persona");
        Json records=Json::array();
        bool saw_knowledge=false;
        if (tail.is_object() && tail.contains("items")) for (const auto& s : tail.at("items")) {
            if (s.at("key")=="childmemory.records") records=s.at("value").at("items");
            if (s.at("key")=="knowledge.published") {
                check(expected_knowledge.is_array(),"unsolicited knowledge"); knowledge.verify(s.at("value").at("items"),expected_knowledge,false); saw_knowledge=true;
            }
        }
        check(saw_knowledge==expected_knowledge.is_array(),"knowledge projection absent");
        std::vector<std::string> actual;
        for (const auto& r : records) {
            actual.push_back(r.at("text").get<std::string>());
            if (r.at("origin")=="main.original") check(r.at("reference")==source,"foreign main original");
            else check(r.at("origin")=="child.note" && r.at("reference").get<std::string>().starts_with(db->conversation()+"/child/"),"untyped child source");
        }
        check(actual==expected.get<std::vector<std::string>>(),"different delivered projection: "+Json(actual).dump());
        if(child_turn.is_object()) {
            for(const auto& marker:child_turn.value("historyHas",Json::array()))
                check(wire.find(marker.get<std::string>())!=wire.npos,"expected previous answer was discarded");
            for(const auto& marker:child_turn.value("historyLacks",Json::array()))
                check(wire.find(marker.get<std::string>())==wire.npos,"old audience or topic leaked into next request");
        }
        for (const auto& marker : markers) if (std::ranges::find(actual,marker)==actual.end())
            check(wire.find(marker)==wire.npos,"private history or source leaked: "+marker);
        entered=true;
        if (holding) {
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while (!release && std::chrono::steady_clock::now()<until) std::this_thread::yield();
        }
        return {{model::Role::assistant,child_turn.is_object() && child_turn.contains("answer") ? child_turn.at("answer").get<std::string>() :
            actual.empty() ? "没有本轮材料。" : actual.front()}};
    }
    // 将语料中的原信别名转为测试实际收到的档案信号，保留接收方副本后缀。
    std::string letter_ref(std::string selector) const {
        if (!selector.starts_with("$")) return selector;
        const auto slash=selector.find('/');
        return knowledge.references.at(selector.substr(0,slash))+
            (slash==selector.npos ? "" : selector.substr(slash));
    }
    // 固定后端先查信，再对实际交付的证据提出签收，逐字展开只检查接收方快照。
    model::Response read_mail(const model::Request& request) {
        for(const auto& marker:mail_case.value("forbidden",Json::array())) for(const auto& message:request.messages)
            check(message.content.find(marker.get<std::string>())==std::string::npos,"unrequested report text reached model");
        const auto status=Json::parse(request.messages.back().content);
        const auto& samples=status.at("items");
        ++mail_step;
        if (mail_step==1) {
            const bool pending=std::ranges::any_of(samples,[](const auto& s){return s.at("key")=="inbox.pending";});
            if (mail_case.contains("pending")) check(pending==mail_case.at("pending").get<bool>(),"wrong inbox notice");
            if (mail_case.value("room",false)) {
                check(!pending && std::ranges::none_of(request.tools,[](const auto& t){return t.name=="memory_search" || t.name=="memory_ack";}),"room accessed inbox");
                for (const auto& m : request.messages) for (const auto& secret : markers) check(m.content.find(secret)==m.content.npos,"room received private mail history");
                reply="收到。"; return {{model::Role::assistant,reply}};
            }
            Json args{{"view","letters"},{"topics",{letter_ref(mail_case.at("selector"))}},{"need","multiple"},
                {"count",mail_case.value("count",3)},{"offset",mail_case.value("offset",0)}};
            return {{model::Role::assistant,"",{{"read","memory_search",args.dump()}}},model::FinishReason::tool_calls};
        }
        if (mail_step==2) {
            const auto s=std::ranges::find_if(samples,[](const auto& x){return x.at("key")=="memory.letters";});
            check(s!=samples.end(),"letter evidence missing");
            const auto& items=s->at("value").at("items"),&answers=mail_case.at("items");
            check(items.size()==answers.size() && items.dump().size()<=2048,"wrong delivered mail count or bytes");
            for(std::size_t i=0;i<answers.size();++i) {
                const auto& item=items[i],&want=answers[i];
                if (want.contains("capture")) {
                    const auto ref=item.at("reference").get<std::string>(), prefix=db->conversation()+"/letter/";
                    check(ref.starts_with(prefix),"wrong receiver profile"); const auto alias=want.at("capture").get<std::string>();
                    letters[alias]=std::stoull(ref.substr(prefix.size())); knowledge.references["$"+alias]=ref;
                }
                check(item.at("reference")==letter_ref(want.at("ref")) && item.at("audience")=="owner" &&
                    item.at("author").get<std::string>().starts_with(db->conversation()+"/child/"),"wrong original sender or reference");
                for(const auto key : {"phase","outcome","mode","summaryKind","summary","acknowledged","partial","origin","subject","digestState","digestEditor"})
                    if (want.contains(key)) check(item.at(key)==want.at(key),std::string("wrong letter ")+key);
                if(want.contains("digest")) {
                    const auto& selection=item.at("digest"); check(selection.size()==want.at("digest").size(),"wrong digest count");
                    const auto& raw=bodies.at(want.at("ref").get<std::string>().substr(1));
                    for(std::size_t j=0;j<selection.size();++j) {
                        const auto& picked=selection[j]; const auto& w=want.at("digest")[j];
                        const auto pos=picked.at("offset").get<std::size_t>(),end=picked.at("endOffset").get<std::size_t>();
                        const auto quote=picked.at("text").get<std::string>();
                        check(end<=raw.size() && end>pos && raw.substr(pos,quote.size())==quote && picked.at("role")==w.at("role") &&
                            quote.find(w.at("contains").get<std::string>())!=quote.npos && picked.at("partial")==Json(quote.size()<end-pos),"wrong extract or original bytes");
                        if(w.contains("save")) originals[w.at("save").get<std::string>()]=pos;
                    }
                } else if(want.contains("digestState")) check(!item.contains("digest"),"unvalidated digest exposed");
                if (want.contains("source")) {
                    const auto& full=bodies.at(want.at("source").get<std::string>());
                    const auto offset=mail_case.value("offset",std::size_t{});
                    const auto part=item.at("text").get<std::string>();
                    check(!part.empty() && full.substr(offset,part.size())==part,"changed exported bytes");
                    if (offset+part.size()<full.size()) check(item.at("nextOffset")==offset+part.size(),"wrong original continuation");
                }
                if (want.contains("export")) check(item.at("noteRef")==letter_ref(want.at("export")) &&
                    item.at("noteOrigin").get<std::string>().starts_with(item.at("author").get<std::string>()+"/note/"),"unbound note copy");
                if (want.contains("bodyRef")) check(item.at("bodyRef")==letter_ref(want.at("bodyRef")),"wrong full-body link");
                if (want.contains("summaryRef")) check(item.at("summaryRef")==letter_ref(want.at("summaryRef")),"wrong sender-summary link");
                if (item.contains("phase")) check(!item.contains("body") && !item.contains("note") && !item.contains("text"),"long report was injected whole");
            }
            if (mail_case.contains("ack")) {
                Json refs=Json::array(); for(const auto& r : mail_case.at("ack")) refs.push_back(letter_ref(r));
                return {{model::Role::assistant,"",{{"ack","memory_ack",Json{{"references",refs}}.dump()}}},model::FinishReason::tool_calls};
            }
        } else {
            const auto result=std::ranges::find_if(request.messages.rbegin(),request.messages.rend(),[](const auto& m){return m.role==model::Role::tool;});
            check(result!=request.messages.rend() && result->content.find(mail_case.value("ackState",std::string{"acknowledged"}))!=std::string::npos,"incorrect acknowledgement receipt");
        }
        reply="已核对这次来信的来源。"; return {{model::Role::assistant,reply}};
    }
    // 保存每一份实际请求同原 Trace 逐件核验，含协作取消后的后台答复。
    void audit() {
        std::lock_guard lock(wire_mutex);
        std::ifstream in(host->trace_file(),std::ios::binary); std::size_t n=0;
        for (std::string line;std::getline(in,line);) {
            auto e=Json::parse(line);
            if (e.at("kind")=="model.prepared" || e.at("kind")=="memory.reflection_prepared")
                check(e.at("wireSha256")==hashes.at(n++),"prepared fields differ");
            if (e.at("kind")=="mail.received" && e.value("category",std::string{})=="memory.letter") ++mail_received;
        }
        check(n==hashes.size(),"missing prepared request");
    }
    // 通过真实实例的绑定调用插件看到的原生接口。
    template<class Action> auto call(const Json& row,Action action) {
        auto plugin=host->plugins().instance(row.value("plugin",std::string{"example.status"}));
        auto tools=plugin->query<dfy_v2_tool_api>(DFY_V2_TOOL_INTERFACE,DFY_V2_TOOL_VERSION);
        check(tools.has_value(),"no fixture capability");
        return plugin->invoke(*tools,[&](const auto&,auto*){return action(service<dfy_v2_agent_memory_api>(DFY_V2_AGENT_MEMORY_INTERFACE,1),
            service<dfy_v2_agent_api>(DFY_V2_AGENT_INTERFACE,DFY_V2_AGENT_PERSONA_VERSION));});
    }
    // 推进被测应用直到指定真实状态，避免依赖线程到达顺序。
    void until(const auto& done) {
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (!done()) { host->advance_plugins(); host->conversation().advance(); std::this_thread::yield(); check(std::chrono::steady_clock::now()<end,"request did not settle"); }
    }
    // 回放一次主人操作、插件调用或真实子回合。
    void step(const Json& row) {
        const auto op=row.at("op").get<std::string>(), session_name=row.value("session",std::string{});
        if (op=="restart") {window_tokens=row.value("window",0u); restart(row); return;}
        if (op=="quiet" || op=="drained") {
            const auto prepared=idle_outcomes("prepared").size();
            for(unsigned i=0;i<(op=="quiet" ? 4u : 25u);++i) {host->advance_plugins(); now+=std::chrono::seconds(1);}
            check(idle_outcomes("prepared").size()==prepared,"idle requested before quiet window or after draining"); return;
        }
        if(op=="schedule") {
            const auto settled=idle_outcomes("all").size(); idle_case=row; entered=false; release=false;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6); bool replied=false;
            while(idle_outcomes("all").size()==settled) {
                host->advance_plugins(); now+=std::chrono::seconds(1);
                if(row.value("foreground",false) && entered && !replied) {
                    host->main_messages()->terminal("interrupt-queue","先回应眼前的交接。"); tests::task_memory::drain(*host);
                    check(db->page(true,0,db->watermark(),1).records.front().envelope.at("text")=="眼前的交接我先听你说。","foreground not delivered");
                    replied=true; release=true;
                }
                check(std::chrono::steady_clock::now()<deadline,"mixed idle queue did not settle: "+failure); std::this_thread::yield();
            }
            const auto actual=idle_outcomes("all").back(),want=row.at("scope").get<std::string>()+"/"+row.value("outcome",std::string{"accepted"});
            check(actual==want && replied==row.value("foreground",false) && failure.empty(),"expected "+want+", got "+actual+" "+failure);
            idle_case=nullptr; return;
        }
        if (op=="invalid_archive") {tests::check_unusable_archive(root,row.at("fault")); return;}
        if (op=="profile") {
            std::ostringstream out,err; app::application_command(*host,"/memory profile",out,err); const auto info=Json::parse(out.str());
            check(info.at("profile")==db->conversation() && info.at("schemaVersion")>=14 && info.at("chatMemoryRevision")>=0,"wrong profile info");
            for(const auto& [label,value]:profile_ids) check((value==db->conversation())==(label==archive),"identity crossed archives or changed on upgrade");
            profile_ids[archive]=db->conversation(); return;
        }
        if (op=="padded") {
            auto ref=knowledge.resolve(row.at("ref")); ref.insert(ref.rfind('/')+1,"000"); knowledge.references[row.at("save").get<std::string>()]=ref; return;
        }
        if(op=="collision") {
            if(row.contains("notes")) check(originals.at(row.at("notes")[0])==originals.at(row.at("notes")[1]),"missing local ID collision");
            else {
                const auto a=knowledge.resolve(row.at("refs")[0]),b=knowledge.resolve(row.at("refs")[1]);
                check(a!=b && a.substr(a.rfind('/'))==b.substr(b.rfind('/')),"expected distinct profiles with equal local ID");
            }
            return;
        }
        if (op=="source") {
            const auto text=row.at("text").get<std::string>(); markers.insert(text);
            const auto alias=row.value("alias",std::string{"@source"});
            const auto m=db->put("terminal",alias,app::user_envelope("send",text,"owner",{}),{{"state","pending"}});
            db->update(m.message.id,{{"state","completed"}});
            source=db->conversation()+"/"+std::to_string(m.message.id); knowledge.references[alias]=source; return;
        }
        if (op=="admin") {knowledge.admin(*host,row,root); return;}
        if (op=="kb") {knowledge.chat(*host,row,++kb_reads); return;}
        if (op=="compare") {
            auto query=row; query["view"]="comparisons"; query["sample"]="memory.comparisons";
            try {knowledge.chat(*host,query,++kb_reads);} catch(const std::exception&) {if(!failure.empty()) throw std::runtime_error(failure); throw;}
            return;
        }
        if (op=="idle" || op=="digest" || op=="digest_release") {
            const auto scope=op=="idle" ? "cross_tool" : "letter_digest";
            if(op=="digest_release") release=true;
            else {idle_case=row; pending_idle=idle_outcomes(scope).size(); entered=false; release=false;}
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6);
            while(idle_outcomes(scope).size()==pending_idle) {
                if(row.value("hold",false) && entered) return;
                host->advance_plugins(); now+=std::chrono::seconds(1); std::this_thread::yield();
                check(std::chrono::steady_clock::now()<deadline,"idle did not settle: "+failure);
            }
            check(failure.empty() && idle_outcomes(scope).back()==row.value("outcome",std::string{"accepted"}),"incorrect idle outcome: "+failure);
            idle_case=nullptr; return;
        }
        if (op=="case_task") {
            auto task=row; task["script"]={"plan","read","change","read","claim"};
            const std::string goal=row.at("id"),alias=row.at("save"),tool=row.at("tool");
            const auto works=tests::task_memory::perform(task,data_dir(),tool.starts_with("example.status/") ? panel : counter,goal,task_requests);
            if (!row.value("approve",true)) return;
            check(works.size()==3,"expected three original receipt events");
            knowledge.traces[alias]=works;
            Json capture{{"topics",{goal}},{"view","tasks"},{"sample","memory.tasks"},{"capture",{{"alias",alias},{"field","readbackRef"}}}};
            knowledge.chat(*host,capture,++kb_reads);
            const auto ref=knowledge.resolve(alias); knowledge.references[alias+"-task"]=ref.substr(0,ref.rfind("/receipt/"));
            return;
        }
        if (op=="verified") {
            Json task{{"id","actual-task"},{"expected","focus"},{"tool","example.status/set_focus"},{"script",{"plan","read","change","read","claim"}}};
            const auto works=tests::task_memory::perform(task,data_dir(),panel,"核实展台面板",task_requests);
            check(works.size()==3 && works.back().at("output")=="focus","verified fixture missing"); return;
        }
        if (op=="grant" || op=="revoke") {
            auto cmd=row.at("command").get<std::string>();
            for (const auto& [alias,ref] : knowledge.references) if(auto pos=cmd.find(alias);pos!=cmd.npos) cmd.replace(pos,alias.size(),ref);
            std::ostringstream out,err;
            app::application_command(*host,cmd,out,err); return;
        }
        if (op=="main") {
            expected=Json::array(); expected_knowledge=nullptr; expect_persona.reset(); host->main_messages()->terminal("main","你还记得这件事吗？");
            until([&]{return !host->conversation().busy();}); return;
        }
        if (op=="pump") {for(int n=0;n<16;++n) host->advance_plugins(); return;}
        if (op=="mail") {
            mail_case=row; mail_step=0; reply.clear(); expect_persona.reset();
            if(row.contains("offsetFrom")) mail_case["offset"]=originals.at(row.at("offsetFrom").get<std::string>());
            if (row.value("room",false)) {host->post_room_event(app::RoomEvent::addressed,"书房","查收新的来信了吗？"); host->advance_plugins();}
            else host->main_messages()->terminal("q"+std::to_string(++mail_reads),"请核对已交付的子 Agent 来信。");
            until([&]{return !host->conversation().busy();});
            check(failure.empty() && !reply.empty(),"mail query failed: "+failure);
            mail_case=nullptr; return;
        }
        if (op=="release" || op=="finish_queued") {
            release=true; until([&]{return !host->subagents().busy(host->plugins().instance("example.status")->generation());});
            if(row.contains("answer")) {
                const auto status=host->subagents().status(host->plugins().instance("example.status")->generation(),sessions.at(session_name));
                check(failure.empty() && status.phase==plugins::SubagentPhase::succeeded && status.answer==row.at("answer"),
                    "unrelated revocation cancelled replacement context: "+failure);
            }
            holding=false; return;
        }
        const bool deny=row.value("deny",false);
        const auto id=sessions.contains(session_name) ? sessions.at(session_name) : 0;
        const auto rc=call(row,[&](const auto& memory,const auto& agent)->dfy_v2_status {
            if (op=="open") {
                const auto preset=row.at("preset").get<unsigned>(); const auto subject=row.at("subject").get<std::string>(), channel=row.value("channel",std::string{});
                std::uint64_t session=0; const auto result=memory.open_bound(preset,view(subject),view(channel),&session,nullptr);
                if (!result) sessions[session_name]=session; else check(!session,"issued a rejected binding");
                return result;
            }
            if (op=="note") {
                const auto event=row.at("event").get<std::string>(), topic=row.value("topic",std::string{"手册"}), body=row.at("text").get<std::string>();
                markers.insert(body);
                std::uint64_t original=0; const auto result=memory.remember(id,view(event),view(topic),view(body),&original,nullptr);
                if (!result && row.contains("same")) check(original==originals.at(row.at("same").get<std::string>()),"retry created a second original");
                if (!result && row.contains("save")) {const auto label=row.at("save").get<std::string>(); originals[label]=original; bodies[label]=body;}
                if (result) check(!original,"rejected write exposed note ID");
                return result;
            }
            if (op=="select") return memory.select_topic(id,view(row.value("topic",std::string{"手册"})),nullptr);
            if (op=="select_kb") return service<dfy_v2_agent_knowledge_api>(DFY_V2_AGENT_KNOWLEDGE_INTERFACE,1).select_topic(id,view(row.at("topic").get<std::string>()),nullptr);
            if (op=="begin" || op=="end") {
                const auto version=row.value("abi",1u);
                const auto& api=service<dfy_v2_agent_letters_api>(DFY_V2_AGENT_LETTERS_INTERFACE,version); std::uint64_t receipt=0;
                check(api.header.interface_version==version,"wrong negotiated letters version");
                const auto key=row.value("key",std::string{}),summary=row.value("summary",std::string{}),segment=row.value("body",std::string{});
                std::string body; for (unsigned i=0;i<row.value("repeat",1u);++i) body+=segment;
                if(row.contains("parts")) for(const auto& part:row.at("parts")) {
                    if(part.is_string()) body+=part.get<std::string>();
                    else for(unsigned i=0;i<part.at("repeat");++i) body+=part.at("text").get<std::string>();
                }
                const auto result=op=="begin" ? api.begin(id,view(key),row.at("mode"),row.value("persona",false),&receipt,nullptr) :
                    api.finish(id,view(summary),view(body),row.contains("note") ? originals.at(row.at("note")) : 0,&receipt,nullptr);
                if (!result && row.contains("save")) {
                    const auto label=row.at("save").get<std::string>();
                    if (letters.contains(label)) check(letters.at(label)==receipt,"duplicate exchange generated another letter");
                    letters[label]=receipt; bodies[label]=body; bodies[label+"-summary"]=summary;
                    knowledge.references["$"+label]=db->conversation()+"/letter/"+std::to_string(receipt);
                }
                if (result) check(!receipt,"rejected letter has receipt");
                return result;
            }
            if (op=="close") return agent.close(id,nullptr);
            if (op=="cancel") {dfy_v2_agent_status status{}; auto rc=agent.status(id,&status,nullptr); return rc ? rc : agent.cancel(id,status.request,nullptr);}
            if (op=="status") {
                dfy_v2_agent_status status{}; auto result=agent.status(id,&status,nullptr);
                if (!result) check(!status.answer.length && status.phase==DFY_V2_AGENT_CANCELLED,"revoked answer still available");
                return result;
            }
            check(op=="run","unknown fixture action"); expected=row.at("expect"); child_turn=row; entered=false; release=false; holding=row.value("hold",false);
            expected_knowledge=row.value("knowledge",Json(nullptr));
            expect_persona=row.contains("persona") ? std::optional(row.at("persona").get<bool>()) : std::nullopt;
            dfy_v2_agent_input input{}; input.prompt=view("按当前授权的主题回复。");
            if (row.value("out",false)) input.flags=DFY_V2_AGENT_PLAYER_FACING;
            std::uint64_t request=0; return agent.submit(id,&input,&request,nullptr);
        });
        check((rc!=DFY_V2_OK)==deny,"unexpected ABI result "+std::to_string(rc)+" for "+op);
        if (op=="run" && !deny) {
            if (row.value("queued",false)) return;
            if (holding) until([&]{return entered.load();});
            else {
                until([&]{return !host->subagents().busy(host->plugins().instance(row.value("plugin",std::string{"example.status"}))->generation());});
                const auto status=host->subagents().status(host->plugins().instance(row.value("plugin",std::string{"example.status"}))->generation(),id);
                check(entered && status.phase==plugins::SubagentPhase::succeeded,"child failed to deliver expected projection: "+failure);
            }
        }
    }
};
}

// 在同一持久档案内重启实际插件并逐步回放跨身份、权限切换和撤权用例。
int wmain(int argc,wchar_t** argv) {
    try {
        if (argc!=5) return 2;
        const fs::path parent=argv[4]; fs::create_directories(parent); fs::path root;
        for(unsigned i=1;;++i) if(fs::create_directory(root=parent/std::to_string(i))) break;
        Fixture fixture{root,argv[1],argv[2]}; fixture.restart();
        std::ifstream in(fs::path(argv[3]),std::ios::binary); unsigned count=0;
        for(std::string line;std::getline(in,line);) {
            auto row=Json::parse(line); ++count;
            try { fixture.step(row); } catch(const std::exception& e) {throw std::runtime_error("case "+std::to_string(count)+": "+e.what());}
        }
        fixture.audit(); check(count && fixture.hosts>=2,"incomplete corpus");
        if (fixture.mail_reads) check(fixture.mail_received>0,"persistent letters bypassed real mailbox");
        std::cout<<count<<" steps, "<<fixture.hosts<<" application instances, "<<fixture.received<<" actual requests, "<<fixture.mail_received
            <<" mailbox receipts, "<<fixture.task_requests<<" additional verified-task requests\n"; return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
