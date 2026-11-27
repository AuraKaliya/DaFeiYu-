#include "application.hpp"
#include "application_commands.hpp"
#include "main_messages.hpp"
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
    Json mail_case;
    std::map<std::string,std::uint64_t> letters;
    std::map<std::string,std::string> bodies;
    std::optional<bool> expect_persona;
    std::string failure,reply;
    unsigned mail_step=0,mail_reads=0,mail_received=0;
    std::atomic<bool> entered=false,release=false;
    bool holding=false;
    unsigned hosts=0,received=0;
    std::shared_ptr<app::MessageStore> db;
    std::unique_ptr<app::Application> host;

    // 重建应用、数据库连接和插件实例，保留原档案与已安装的插件包。
    void restart() {
        if (host) audit();
        host.reset(); db=std::make_shared<app::MessageStore>(root);
        app::ApplicationRunConfig config; config.trace_directory=root/"logs";
        host=std::make_unique<app::Application>(model::Config{},root/"packages",root,
            model::Backend{[this](const model::Config& cfg,const model::Request& request){
                try {return answer(cfg,request);} catch(const std::exception& e){failure=e.what(); throw;}
            }},config);
        host->bind_messages(db,"child-"+std::to_string(++hosts),nullptr);
        if (hosts==1) { host->plugins().install(panel); host->plugins().install(counter); }
        for (const auto plugin : {"example.status","test.tools"}) check(host->plugins().load(plugin)==0,"plugin not loaded");
        hashes.clear();
    }
    // 核查实际请求、受众和来源；保留一次可控的迟到答复供在途撤权验收。
    model::Response answer(const model::Config& cfg,const model::Request& request) {
        const auto wire=model::encode_request_fields(request,cfg.thinking_enabled());
        hashes.push_back(platform::sha256(wire)); ++received;
        if (mail_case.is_object()) return read_mail(request);
        if (expect_persona) check((request.messages.front().content.find("你叫大肥鱼")!=std::string::npos)==*expect_persona,"wrong startup persona");
        Json records=Json::array();
        const auto tail=Json::parse(request.messages.back().content,nullptr,false);
        if (tail.is_object() && tail.contains("items")) for (const auto& s : tail.at("items"))
            if (s.at("key")=="childmemory.records") records=s.at("value").at("items");
        std::vector<std::string> actual;
        for (const auto& r : records) {
            actual.push_back(r.at("text").get<std::string>());
            if (r.at("origin")=="main.original") check(r.at("reference")==source,"foreign main original");
            else check(r.at("origin")=="child.note" && r.at("reference").get<std::string>().starts_with(db->conversation()+"/child/"),"untyped child source");
        }
        check(actual==expected.get<std::vector<std::string>>(),"different delivered projection: "+Json(actual).dump());
        for (const auto& marker : markers) if (std::ranges::find(actual,marker)==actual.end())
            check(wire.find(marker)==wire.npos,"private history or source leaked: "+marker);
        entered=true;
        if (holding) {
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while (!release && std::chrono::steady_clock::now()<until) std::this_thread::yield();
        }
        return {{model::Role::assistant,actual.empty() ? "没有本轮材料。" : actual.front()}};
    }
    // 将语料中的原信别名转为测试实际收到的档案信号，保留接收方副本后缀。
    std::string letter_ref(std::string selector) const {
        if (!selector.starts_with("$")) return selector;
        const auto slash=selector.find('/');
        return db->conversation()+"/letter/"+std::to_string(letters.at(selector.substr(1,slash==selector.npos ? slash : slash-1)))+
            (slash==selector.npos ? "" : selector.substr(slash));
    }
    // 固定后端先查信，再对实际交付的证据提出签收，逐字展开只检查接收方快照。
    model::Response read_mail(const model::Request& request) {
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
                    check(ref.starts_with(prefix),"wrong receiver profile"); letters[want.at("capture").get<std::string>()]=std::stoull(ref.substr(prefix.size()));
                }
                check(item.at("reference")==letter_ref(want.at("ref")) && item.at("audience")=="owner" &&
                    item.at("author").get<std::string>().starts_with(db->conversation()+"/child/"),"wrong original sender or reference");
                for(const auto key : {"phase","outcome","mode","summaryKind","summary","acknowledged","partial","origin","subject"})
                    if (want.contains(key)) check(item.at(key)==want.at(key),std::string("wrong letter ")+key);
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
        std::ifstream in(host->trace_file(),std::ios::binary); std::size_t n=0;
        for (std::string line;std::getline(in,line);) {
            auto e=Json::parse(line);
            if (e.at("kind")=="model.prepared") check(e.at("wireSha256")==hashes.at(n++),"prepared fields differ");
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
        const auto op=row.at("op").get<std::string>(), alias=row.value("session",std::string{});
        if (op=="restart") { restart(); return; }
        if (op=="source") {
            const auto text=row.at("text").get<std::string>(); markers.insert(text);
            const auto m=db->put("terminal","original",app::user_envelope("send",text,"owner",{}),{{"state","pending"}});
            db->update(m.message.id,{{"state","completed"}});
            source=db->conversation()+"/"+std::to_string(m.message.id); return;
        }
        if (op=="grant" || op=="revoke") {
            auto cmd=row.at("command").get<std::string>();
            if (auto pos=cmd.find("@source");pos!=cmd.npos) cmd.replace(pos,7,source);
            std::ostringstream out,err;
            app::application_command(*host,cmd,out,err); return;
        }
        if (op=="main") {
            expected=Json::array(); expect_persona.reset(); host->main_messages()->terminal("main","你还记得这件事吗？");
            until([&]{return !host->conversation().busy();}); return;
        }
        if (op=="pump") {for(int n=0;n<16;++n) host->advance_plugins(); return;}
        if (op=="mail") {
            mail_case=row; mail_step=0; reply.clear(); expect_persona.reset();
            if (row.value("room",false)) {host->post_room_event(app::RoomEvent::addressed,"书房","查收新的来信了吗？"); host->advance_plugins();}
            else host->main_messages()->terminal("q"+std::to_string(++mail_reads),"请核对已交付的子 Agent 来信。");
            until([&]{return !host->conversation().busy();});
            check(failure.empty() && !reply.empty(),"mail query failed: "+failure);
            mail_case=nullptr; return;
        }
        if (op=="release") {
            release=true; until([&]{return !host->subagents().busy(host->plugins().instance("example.status")->generation());});
            holding=false; return;
        }
        const bool deny=row.value("deny",false);
        const auto id=sessions.contains(alias) ? sessions.at(alias) : 0;
        const auto rc=call(row,[&](const auto& memory,const auto& agent)->dfy_v2_status {
            if (op=="open") {
                const auto preset=row.at("preset").get<unsigned>(); const auto subject=row.at("subject").get<std::string>(), channel=row.value("channel",std::string{});
                std::uint64_t session=0; const auto result=memory.open_bound(preset,view(subject),view(channel),&session,nullptr);
                if (!result) sessions[alias]=session; else check(!session,"issued a rejected binding");
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
            if (op=="begin" || op=="end") {
                const auto& api=service<dfy_v2_agent_letters_api>(DFY_V2_AGENT_LETTERS_INTERFACE,1); std::uint64_t receipt=0;
                const auto key=row.value("key",std::string{}),summary=row.value("summary",std::string{}),segment=row.value("body",std::string{});
                std::string body; for (unsigned i=0;i<row.value("repeat",1u);++i) body+=segment;
                const auto result=op=="begin" ? api.begin(id,view(key),row.at("mode"),row.value("persona",false),&receipt,nullptr) :
                    api.finish(id,view(summary),view(body),row.contains("note") ? originals.at(row.at("note")) : 0,&receipt,nullptr);
                if (!result && row.contains("save")) {
                    const auto label=row.at("save").get<std::string>();
                    if (letters.contains(label)) check(letters.at(label)==receipt,"duplicate exchange generated another letter");
                    letters[label]=receipt; bodies[label]=body;
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
            check(op=="run","unknown fixture action"); expected=row.at("expect"); entered=false; release=false; holding=row.value("hold",false);
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
        std::cout<<count<<" steps, "<<fixture.hosts<<" application instances, "<<fixture.received<<" actual requests, "<<fixture.mail_received<<" mailbox receipts\n"; return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
