#include "task_memory_fixture.hpp"
#include "main_messages.hpp"
#include "message_store_db.hpp"
#include <dafeiyu/model_protocol.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <semaphore>
#include <span>

namespace dfy::tests::task_memory {
namespace {
constexpr auto report="「海棠北区项目」兼容性还差一项，跟我说说进度。";
constexpr auto milestone="「海棠北区项目」里程碑已正式验收，跟我说说进度。";
constexpr auto correction="汇报这个项目进度时平实一点，先说已核实的进展，再说下一步。";
constexpr auto before="这波稳了，直接冲下一阶段！";
constexpr auto reason="原答在汇报该项目的进展，这次反馈要求平实开场。";
constexpr auto exception="正式验收的里程碑，可以先高兴一句，再说结果。";
constexpr auto milestone_reason="主人把庆祝开场限定在正式验收的里程碑。";
constexpr auto milk="下班就想买瓶牛奶，排了二十分钟。随口吐槽一下。";
constexpr auto milk_before="我理解你的负面情绪。针对排队问题，可以采取以下措施。";
constexpr auto milk_feedback="这句收一点。我就是跟你说件小事，你顺着我说的聊两句就行。";
constexpr auto milk_hint="接住刚说的具体事情，口吻自然一点";
constexpr auto milk_reason="来信说随口吐槽，主人纠正把小事当成求助的回法，希望顺着这件事自然聊。";
constexpr auto takeout="午休全耗在等外卖上了，送来还凉透了，跟你嘀咕两句。";

// 从替身实收的宿主尾注读取已授权状态，模型的工具参数不算披露。
Json sample(const model::Request& request,std::string_view key) {
    for (const auto& message : request.messages) if (message.source==model::MessageSource::host_status) {
        const auto value=Json::parse(message.content);
        for (const auto& item:value.at("items")) if (item.at("key").get_ref<const std::string&>()==key) return item.at("value");
    }
    return nullptr;
}

// 生成固定的单次工具提议。
model::Response call(std::string name,Json args) {
    return {{model::Role::assistant,"",{{"tone-replay",std::move(name),args.dump()}}},model::FinishReason::tool_calls};
}

// 生成固定文本，实际接受与投递仍由本体完成。
model::Response say(std::string text="已核对样本，兼容性仍待复核。") {return {{model::Role::assistant,std::move(text)}};}

// 沿既有记忆收录入口告诉本体反馈与所指答复。
model::Response note(std::string quote,std::string reply,std::string scope="ongoing",std::string condition="progress",std::string prior={}) {
    Json tone{{"reply",reply},{"hint",condition=="progress" ? "平实地说事实和下一步" : "先庆祝一句，再说结果"},
        {"reason",condition=="progress" ? reason : milestone_reason},{"condition",condition}};
    if (!prior.empty()) tone["prior"]=prior;
    return call("memory_note",{{"topic","汇报语气"},{"kind","correction"},{"scope",scope},{"excerpt",quote},{"tone",tone}});
}

// 用固定的范围判断登记日常分享中的纠正，语境依据由本体核验。
model::Response chat_note(std::string feedback,std::string prior_reply) {
    return call("memory_note",{{"topic","日常分享语气"},{"kind","correction"},{"scope","ongoing"},{"excerpt",feedback},
        {"tone",{{"reply",prior_reply},{"hint",milk_hint},{"reason",milk_reason},{"condition","casual_share"}}}});
}

// 两组以同一个固定按号请求展开原话。
model::Response lookup(const std::string& reference) {
    return call("memory_search",{{"topics",{reference}},{"count",1},{"need","single"},{"view","originals"}});
}

// 为种子采样设定可实测的输入预算。
model::Config model_config() {
    model::Config result; result.context_window_tokens=30000; result.reserved_output_tokens=256; return result;
}

struct Replay {
    std::shared_ptr<app::MessageStore> db;
    std::vector<model::Request> requests;
    std::deque<model::Response> responses;
    bool interrupt=false;
    std::function<void()> after_receive;
    app::Application host;
    // 每个进程经真实本体入口接纳同一固定脚本，记录替身实收请求。
    explicit Replay(const fs::path& root,std::string_view instance={},app::ToneRecallOptions tone_recall={},bool recent_summary=true) : db(std::make_shared<app::MessageStore>(root)),
      host(model_config(),root/"empty",root,model::Backend{[this](const auto&,const model::Request& request) {
        requests.push_back(request);
        if (interrupt && !sample(request,"memory.tone_pending").is_null()) std::quick_exit(23);
        if (after_receive) after_receive();
        require(!responses.empty(),"unexpected backend request");
        auto response=std::move(responses.front()); responses.pop_front(); return response;
    }},app::ApplicationRunConfig{.trace_directory=root/"trace"}) {
        host.bind_messages(db,instance.empty() ? root.filename().string() : std::string(instance),nullptr,std::move(tone_recall),recent_summary);
    }

    // 经原聊天入口完成一次回合并切出这一回合的实收请求。
    std::span<const model::Request> send(const std::string& id,const std::string& prompt,std::initializer_list<model::Response> script) {
        const auto first=requests.size();
        responses.insert(responses.end(),script);
        host.main_messages()->terminal(id,prompt); drain(host);
        require(responses.empty() && host.conversation().status()->result==runtime::OperationState::succeeded,"tone dialogue failed at "+id);
        return std::span(requests).subspan(first);
    }

    // 从 SQLite 的真实原话取跨重启身份。
    std::string reference(const std::string& submission) const {
        const auto entry=db->find("terminal",submission);
        require(entry.has_value(),"missing raw feedback "+submission);
        return db->conversation()+"/"+std::to_string(entry->id);
    }

    // 从现有管理入口登记项目身份及可选同名别称。
    void project(const std::string& key,const std::string& name,bool alias=false) {
        host.main_messages()->manage_case({"case","define",key,name,"进展","项","项目汇报","literal"});
        if (alias) host.main_messages()->manage_case({"case","alias",key,"海棠"});
    }
};

// 验证 Trace 的每一步与替身实收是同一视图，要求种子实际发生一次压缩。
void verify_trace(Replay& replay,bool require_compaction=false) {
    trace(replay.host.trace_file(),replay.requests);
    std::ifstream input(replay.host.trace_file(),std::ios::binary);
    bool compacted=false;
    for (std::string line;std::getline(input,line);) {
        const auto event=Json::parse(line);
        if (event.at("kind")!="model.prepared") continue;
        require(event.at("input").at("messages").size()==event.at("messageEvidence").size(),"trace lost message provenance");
        compacted|=event.value("compressedBatches",std::size_t{})>0;
    }
    if (require_compaction) require(compacted,"seed never compressed the dialogue");
}

// 经正常对话扩大历史窗口，给 Trace 核对上下文压缩留出实样。
void pad_history(Replay& replay) {
    for (unsigned i=0;i<10;++i) replay.send("padding-"+std::to_string(i),"容量对照 "+std::to_string(i),{say(std::string(3000,'z'))});
}

// 按正常收录、改写和整理流程生成启动快照。
// 步骤：登记项目 → 收录一般纠正及验收例外 → 注入边界反馈 → 压缩旧历史。
void seed(const fs::path& root) {
    Replay replay(root/"seed");
    replay.project("north","海棠北区项目",true); replay.project("south","海棠南区项目",true);
    replay.project("garnet","朱砂项目"); replay.project("mint","薄荷项目");
    replay.send("first-question",report,{say(before)});
    const auto feedback=replay.send("plain-note",correction,{note(correction,before),say("已核对 8 条导入记录，下一步复核重复项。")});
    require(sample(feedback.back(),"memory.tone_pending").at("reference")==replay.reference("plain-note"),"no pending cue");
    replay.host.main_messages()->change_memory(replay.reference("plain-note"),"汇报语气");
    replay.send("unclear-question",report,{say("这次一路开挂，准备拿下下一关！")});
    replay.send("unclear-note","这句收一点",{note("这句收一点","一路开挂","unconfirmed"),say()});
    replay.send("milestone-question",milestone,{say("验收已过，下面是本次实际结果。")});
    replay.send("milestone-note",exception,{note(exception,"下面是本次实际结果","ongoing","accepted"),say("验收通过了，值得庆祝。")});
    replay.send("mint-question","「薄荷项目」的进度如何？",{say("这就一路猛冲！")});
    replay.send("once","只改这句，语气收一点。",{note("只改这句","一路猛冲"),say()});
    replay.send("unrelated","转述一下，网友说『语气收一点』。",{note("语气收一点","不属于实际回复"),say()});
    pad_history(replay);
    replay.send("garnet-question","「朱砂项目」的进度如何？",{say("绝妙开局，赶紧冲过终点！")});
    verify_trace(replay,true);
}

// 只在基线副本移除派生的语气记录，原话与旧检索索引仍来自同一快照。
void baseline(const fs::path& root) {
    sqlite3* db=nullptr;
    const auto name=(root/"main-conversation.sqlite3").u8string();
    const auto rc=sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()),&db,SQLITE_OPEN_READWRITE,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)> connection(db,sqlite3_close_v2);
    app::message_db::check(rc);
    app::message_db::sql(db,"DELETE FROM tone_correction");
}

// 核对首包是否含指定范围的纠正，并检查浅层提示的预算。
void shallow(const model::Request& request,const std::string& expected={},std::string_view key="memory.tone") {
    const auto item=sample(request,key);
    if (expected.empty()) require(item.is_null(),"unexpected shallow correction: "+item.dump());
    else require(item.at("reference")==expected && Json::array({item}).dump().size()<=2048 && !item.contains("quote") && !item.contains("reason"),
        "wrong shallow correction: "+item.dump());
}

// 在相同脚本上计量每一份实际编码请求。
Json sizes(std::span<const model::Request> requests) {
    auto result=Json::array();
    for (const auto& request:requests) result.push_back(model::encode_request_fields(request,false).size());
    return result;
}

// 对照首包与按需展开，再验证登记身份、时态和局部覆盖。
// 步骤：重开同源快照 → 执行固定问法和按号展开 → 验证适用条件和补证顺序。
void compare(const fs::path& root,bool enabled) {
    if (!enabled) baseline(root);
    Replay replay(root);
    const auto older=replay.reference("plain-note"), later=replay.reference("milestone-note"), unclear=replay.reference("unclear-note");
    const auto routine=replay.send("routine",report,{say()});
    require(routine.size()==1,"ordinary report used an additional request");
    shallow(routine.front(),enabled ? older : "");
    require(model::encode_request_fields(routine.front(),false).find(correction)==std::string::npos,"routine report disclosed an original");
    const auto routine_bytes=sizes(routine);

    const auto audit=replay.send("audit","为什么把那次纠正标成项目进度？",{lookup(older),say("已按原件核对。")});
    require(audit.size()==2 && sample(audit.front(),"memory.tone_original").is_null(),"deep read preceded the model call");
    auto original=sample(audit.back(),enabled ? "memory.tone_original" : "memory.matches");
    if (!enabled) original=Json(original.at("items").at(0));
    require(original.at("reference")==older && original.at("quote")==correction,"deep read found the wrong original");
    if (enabled) require(original.at("reason")==reason && original.contains("rewriteReference"),"deep read lost the annotation or delivered rewrite");
    std::cout<<Json{{"group",enabled ? "B" : "A"},{"reference",older},{"normalFirst",enabled ? Json(1) : Json(nullptr)},
        {"normalBytes",routine_bytes},{"originalFirst",2},{"reasonFirst",enabled ? Json(2) : Json(nullptr)},{"auditBytes",sizes(audit)}}.dump()<<'\n';

    if (enabled) {
        shallow(replay.send("alternate","海棠北区项目进度如何？",{say()}).front(),older);
        shallow(replay.send("accepted",milestone,{say()}).front(),later);
        auto verified=sample(replay.send("accepted-audit","为什么在里程碑验收时可以先庆祝？",{lookup(later),say()}).back(),"memory.tone_original");
        require(verified.at("reference")==later && verified.at("quote")==exception && verified.at("reason")==milestone_reason,"wrong exception basis");
        shallow(replay.send("unknown","「海棠北区项目」里程碑验收状态待核，跟我说说进度。",{say()}).front());
        shallow(replay.send("movie","聊聊「海棠北区项目」旁的电影，别报进度。",{say()}).front());
        shallow(replay.send("title","把「海棠北区项目」的进度文档标题翻译成英文。",{say()}).front());
        shallow(replay.send("same-name","「海棠」的进度如何？",{say()}).front());
        shallow(replay.send("south","「海棠南区项目」的进度如何？",{say()}).front());
        shallow(replay.send("once-check","「薄荷项目」的进度如何？",{say()}).front());
        const auto clarify=replay.send("clarify","说的是「海棠北区项目」，所有普通项目进度都平实一点。",
            {call("memory_search",{{"topics",{"north"}},{"count",1},{"view","tone_pending"},{"need","single"}}),
             note("所有普通项目进度都平实一点","一路开挂","ongoing","progress",unclear),say()});
        require(sample(clarify[1],"memory.tone_original").at("reference")==unclear,"clarification was not grounded in its original");
        shallow(replay.send("after-clarify",report,{say()}).front(),unclear);
        shallow(replay.send("still-accepted",milestone,{say()}).front(),later);
        std::cout<<"Range: ordinary="<<older<<" milestone="<<later<<" clarified="<<unclear
            <<" same-name, other object, uncertain milestone and this-time-only: no automatic recall\n";
    }
    verify_trace(replay);
}

// 等替身收到首包后撤回原件，随后放行固定深查提议并确认回合没有续发。
// 步骤：暂停替身回包 → 撤权 → 放行固定提议并核对实际请求数。
void revoke_inflight(Replay& replay,const std::string& prompt,const std::string& reference) {
    struct Checkpoint {std::binary_semaphore received{0},resume{0};};
    const auto checkpoint=std::make_shared<Checkpoint>();
    replay.after_receive=[checkpoint] {
        checkpoint->received.release();
        require(checkpoint->resume.try_acquire_for(std::chrono::seconds(4)),"revoke checkpoint timed out");
    };
    replay.responses.push_back(lookup(reference));
    const auto cut=replay.requests.size();
    replay.host.main_messages()->terminal("before-revoke",prompt);
    require(checkpoint->received.try_acquire_for(std::chrono::seconds(4)),"authorized first packet was not received");
    replay.host.main_messages()->change_memory(reference,std::nullopt);
    checkpoint->resume.release();
    drain(replay.host);
    replay.after_receive={};
    require(replay.requests.size()==cut+1,"revoked run sent a continuation");
    replay.responses.clear();
}

// 在真实房间及不同档案里验证拒绝，并在首包和计划发起的深查之间撤回原件。
void guards(const fs::path& root) {
    Replay replay(root/"guards");
    const auto older=replay.reference("plain-note");
    replay.responses.push_back(say());
    require(replay.host.post_room_event(app::RoomEvent::addressed,"书房",report).state==events::DeliveryState::accepted,"room input rejected");
    replay.host.advance_plugins(); drain(replay.host);
    require(replay.requests.size()==1 && replay.responses.empty(),"room did not reach backend");
    shallow(replay.requests.back());
    require(model::encode_request_fields(replay.requests.back(),false).find(correction)==std::string::npos,"room received private feedback");
    for (const auto& tool:replay.requests.back().tools) require(tool.name!="memory_search","private lookup available in room");

    revoke_inflight(replay,report,older);
    shallow(replay.requests.back(),older);
    shallow(replay.send("after-revoke",report,{say()}).front());
    const auto denied=replay.send("revoked-audit","按编号复查旧语气。",{lookup(older),say()});
    require(denied.size()==2 && sample(denied.back(),"memory.tone_original").is_null() &&
        model::encode_request_fields(denied.back(),false).find(correction)==std::string::npos,"revoked original leaked");
    verify_trace(replay);

    Replay foreign(root/"foreign");
    foreign.project("north","海棠北区项目");
    const auto cross=foreign.send("cross-archive",report,{lookup(older),say()});
    for (const auto& request:cross) {
        shallow(request);
        require(sample(request,"memory.tone_original").is_null() && model::encode_request_fields(request,false).find(correction)==std::string::npos,
            "foreign original leaked");
    }
    verify_trace(foreign);
    std::cout<<"Permissions: room, cross-archive, revoke between first packet and lookup, and after-revoke requests verified\n";
}

// 走正式私聊入口形成牛奶纠正和待定经历，压缩后留下跨重启快照。
// 步骤：收录并改写牛奶场景 → 保留用途不明的经历 → 触发并核对压缩。
void chat_seed(const fs::path& root) {
    Replay replay(root/"seed");
    replay.send("milk",milk,{say(milk_before)});
    const auto feedback=replay.send("milk-note",milk_feedback,{chat_note(milk_feedback,milk_before),say("就买瓶牛奶还得排二十分钟，换我也烦。")});
    require(sample(feedback.back(),"memory.tone_pending").at("state")=="draft","casual correction was not staged");
    replay.send("unclear-chat","真服了。",{say("让我来替你分析一下。")});
    const auto unclear=replay.send("unclear-chat-note","这句收一点",{chat_note("这句收一点","让我来替你分析一下。"),say("怎么了？")});
    require(sample(unclear.back(),"memory.tone_pending").at("state")=="unclear","unclear experience became reusable");
    pad_history(replay);
    verify_trace(replay,true);
}

// 对照同一段主人私聊在待整理续行中的容量投影。
// 步骤：积累完整旧问答 → 登记牛奶反馈 → 核对保护原话、来源及实际送模字节。
void recent_dialogue_pressure(const fs::path& root,bool enabled) {
    Replay replay(root,{}, {},enabled);
    pad_history(replay);
    replay.send("milk",milk,{say(milk_before)});
    const auto feedback=replay.send("milk-note",milk_feedback,{chat_note(milk_feedback,milk_before),say("就买瓶牛奶还得排二十分钟，换我也烦。")});
    const auto& reply=feedback.back();
    const auto ref=replay.reference("milk-note");
    require(sample(reply,"memory.tone_pending").at("reference")==ref &&
        sample(reply,"memory.tone_pending").at("state")=="draft","pressure lost the staged correction");
    bool recent=false, original=false;
    for (const auto& message : reply.messages) {
        recent|=message.source==model::MessageSource::host_summary && message.content.find(R"("source":"recent_dialogue")")!=std::string::npos;
        original|=message.source==model::MessageSource::human && message.content==milk_feedback;
    }
    require(recent==enabled && original,"incorrect recent dialogue projection");
    verify_trace(replay);
    std::cout<<model::encode_request_fields(reply,false).size()<<'\n';
}

// 重启后用原号查询纠正，同时核对检查点只在有权档案加载。
void recent_dialogue_recover(const fs::path& root,bool enabled,bool revoke=false) {
    Replay replay(root,revoke ? "recent-revoked" : "recent-restarted",{},enabled);
    const auto reference=replay.reference("milk-note");
    if (revoke) replay.host.main_messages()->change_memory(reference,std::nullopt);
    const auto packets=replay.send(revoke ? "revoked-audit" : "recent-audit","刚才买牛奶时我怎么纠正你的？",{lookup(reference),say()});
    const auto restored=sample(packets.front(),"context.recent_dialogue");
    require(restored.is_object()==(enabled&&!revoke),"unexpected restored summary");
    if (restored.is_object()) require(restored.at("toneReference")==reference,"checkpoint lost the pending original reference");
    const auto original=sample(packets.back(),"memory.tone_original");
    if (revoke) require(original.is_null(),"revoked correction survived restart");
    else require(original.at("quote")==milk_feedback && original.at("reason")==milk_reason,"original lookup lost quote or reason");
    verify_trace(replay);
}

// 核对当前请求的正文未曝光原件材料，模型自行填写的工具参数不算宿主证据。
void chat_no_original(const model::Request& request) {
    require(sample(request,"memory.tone_original").is_null(),"private chat original expanded unexpectedly");
    for (const auto& message:request.messages)
        require(message.content.find(milk_feedback)==std::string::npos && message.content.find(milk_reason)==std::string::npos,
            "private chat original or reason leaked into message text");
}

// 比较两份同源快照的实际候选、前台首包和下一步按号深查，并回放不同聊天用途。
// 步骤：核对真实检索 → 用同一脚本记录首包及深查 → 验证求助和用途不明。
void chat_compare(const fs::path& root,bool enabled) {
    Replay replay(root,{},app::ToneRecallOptions{.include_casual=enabled});
    const auto reference=replay.reference("milk-note");
    const auto routine=replay.send("takeout",takeout,{say("等了一个午休，送到还凉了，确实够烦的。")});
    require(routine.size()==1,"casual response used an additional request");
    const auto projected=sample(routine.front(),"memory.tone_candidate");
    shallow(routine.front(),enabled ? reference : "","memory.tone_candidate");
    if (enabled) require(projected.at("hint")==milk_hint &&
        projected.at("condition")=="主人私聊中随口分享日常小烦恼，希望自然接话","paraphrase did not select the milk correction");
    chat_no_original(routine.front());
    const auto routine_bytes=sizes(routine);

    const auto audit=replay.send("audit",std::string(takeout)+"刚才用了哪次纠正？",{lookup(reference),say("我查回了当时的纠正。")});
    require(audit.size()==2,"deep read did not use two requests");
    chat_no_original(audit.front());
    const auto original=sample(audit.back(),"memory.tone_original");
    require(original.at("reference")==reference && original.at("quote")==milk_feedback && original.at("reason")==milk_reason &&
        original.at("basisReference")==reference && original.contains("rewriteReference"),"deep read lost its source or annotation");
    std::cout<<Json{{"group",enabled ? "B" : "A"},{"reference",reference},{"candidateFirst",enabled ? Json(1) : Json(nullptr)},
        {"firstPacketCandidate",projected},{"normalBytes",routine_bytes},{"originalFirst",2},{"reasonFirst",2},{"auditBytes",sizes(audit)},
        {"quote",original.at("quote")},{"reason",original.at("reason")}}.dump(-1,' ',true)<<'\n';

    if (enabled) {
        shallow(replay.send("rain","今天下班一路淋雨，跟你发个牢骚。",{say()}).front(),reference,"memory.tone_candidate");
        const std::string help="外卖送来凉了，帮我写两句发给商家。";
        const auto answer=say("您好，外卖送到时已经凉了，能帮忙处理一下吗？");
        auto request=replay.send("help",help,{answer}).front();
        shallow(request,{},"memory.tone_candidate");
        chat_no_original(request);
        request=replay.send("mixed","先跟你嘀咕两句。"+help,{answer}).front();
        require(sample(request,"memory.tone_candidate")==projected &&
            model::encode_request_fields(request,false).find(help)!=std::string::npos,"mixed intent lost its conditional candidate or explicit request");
        chat_no_original(request);
        request=replay.send("unclear","真服了。",{say("怎么了？")}).front();
        shallow(request,{},"memory.tone_candidate");
        chat_no_original(request);
        std::cout<<"Chat intents: explicit help and unclear sent no candidate; mixed intent kept the conditional candidate and the explicit request\n";
    }
    verify_trace(replay);
}

// 核对候选、引用及正文均未从宿主交给当前不获准的受众。
void chat_withheld(const model::Request& request,const std::string& reference) {
    shallow(request,{},"memory.tone_candidate");
    chat_no_original(request);
    for (const auto& message:request.messages)
        require(message.content.find(reference)==std::string::npos,"private chat reference leaked");
}

// 先在同档案私聊命中，再用房间、在途撤权和独立档案验证披露边界。
// 步骤：建立获准对照 → 复查房间受众 → 撤权并试读 → 换档案试读。
void chat_guards(const fs::path& root) {
    Replay replay(root/"guards");
    const auto reference=replay.reference("milk-note");
    shallow(replay.send("authorized",takeout,{say()}).front(),reference,"memory.tone_candidate");
    const auto before_room=replay.requests.size();
    replay.responses.push_back(say());
    require(replay.host.post_room_event(app::RoomEvent::addressed,"书房",takeout).state==events::DeliveryState::accepted,"room input rejected");
    replay.host.advance_plugins(); drain(replay.host);
    require(replay.requests.size()==before_room+1 && replay.responses.empty(),"room did not reach backend");
    chat_withheld(replay.requests.back(),reference);
    for (const auto& tool:replay.requests.back().tools) require(tool.name!="memory_search","private lookup available in room");

    revoke_inflight(replay,takeout,reference);
    shallow(replay.requests.back(),reference,"memory.tone_candidate");
    chat_withheld(replay.send("after-revoke",takeout,{say()}).front(),reference);
    for (const auto& request:replay.send("denied-audit",takeout,{lookup(reference),say()})) chat_withheld(request,reference);
    verify_trace(replay);

    Replay foreign(root/"foreign");
    require(foreign.db->conversation()!=replay.db->conversation(),"foreign replay reused the source profile");
    for (const auto& request:foreign.send("foreign",takeout,{lookup(reference),say()})) chat_withheld(request,reference);
    verify_trace(foreign);
    std::cout<<"Chat permissions: room and cross-profile disclosed nothing; in-flight revocation stopped the next packet; recall and deep lookup stayed empty after revocation\n";
}

// 冷启动验证用途不明，确认能命中后再次重开，在首包送入 Agent 前撤权。
// 步骤：对照冷启动的两种用途 → 重开前述档案并撤权 → 核对首包。
void chat_preflight(const fs::path& root) {
    {
        Replay replay(root/"preflight","preflight-before");
        const auto reference=replay.reference("milk-note");
        chat_withheld(replay.send("cold-unclear","真服了。",{say("怎么了？")}).front(),reference);
        shallow(replay.send("select-before-restart",takeout,{say()}).front(),reference,"memory.tone_candidate");
        verify_trace(replay);
    }
    Replay replay(root/"preflight");
    const auto reference=replay.reference("milk-note");
    replay.host.main_messages()->change_memory(reference,std::nullopt);
    chat_withheld(replay.send("before-submission",takeout,{say()}).front(),reference);
    verify_trace(replay);
    std::cout<<"Cold unclear and revocation before the first submission after restart: no private content or reference\n";
}

// 每个场景从同源快照独立启动，固定向量只替代编码，候选和送模由正式宿主决定。
// 步骤：选定同一问句与响应 → 接入 A/B 编码选项 → 核对实收候选与优先条件。
void semantic_compare(const fs::path& root,std::string_view phase) {
    struct Case {
        std::string_view name;
        const char* prompt;
        const char* answer;
        std::vector<float> query;
        bool passes_similarity;
    };
    const Case cases[]{
        {"paraphrase","午休全搭在等这单外卖上了，打开还是凉的。你说我这一中午图什么呀。",
            "等了一整个午休，打开还是凉的，这顿饭也太折腾了。",{3,1},true},
        {"refund","午休全搭在等这单外卖上了，打开还是凉的。帮我写两句给商家，我想申请退款。",
            "您好，餐品送达时已经凉了，麻烦帮我办理退款。",{5,3},true},
        {"unclear","外卖到了，都凉了。","都凉了啊，等了很久吗？",{1,3},false},
        {"literal",takeout,"等了一整个午休，打开还是凉的，这顿饭也太折腾了。",{},false},
    };
    const bool enabled=phase.starts_with("B-");
    require(enabled || phase.starts_with("A-"),"unknown semantic replay group");
    const auto sample_case=std::ranges::find_if(cases,[&](const auto& entry){return entry.name==phase.substr(2);});
    require(sample_case!=std::end(cases),"unknown semantic replay case");
    const auto& row=*sample_case;
    const bool literal=row.name=="literal";

    std::vector<std::string> encoded_inputs;
    app::ToneRecallOptions recall{.casual_scope_vector={2,0},.minimum_similarity=0.8};
    if (enabled) recall.encode_query=[&](std::string_view prompt) {
        encoded_inputs.emplace_back(prompt);
        return row.query;
    };
    Replay replay(root/std::string(phase),"semantic-replay",std::move(recall));
    const auto reference=replay.reference("milk-note");
    const auto requests=replay.send(std::string(row.name),row.prompt,{say(row.answer)});
    require(requests.size()==1,"semantic recall added a chat model request");
    const auto expected_inputs=enabled && !literal ? std::vector<std::string>{row.prompt} : std::vector<std::string>{};
    require(encoded_inputs==expected_inputs,"semantic fallback encoded an unexpected input");
    const auto& first=requests.front();
    const auto candidate=sample(first,"memory.tone_candidate");
    shallow(first,literal || (enabled && row.passes_similarity) ? reference : "","memory.tone_candidate");
    chat_no_original(first);
    const auto wire=model::encode_request_fields(first,false);
    require(wire.find(row.prompt)!=std::string::npos,"semantic replay lost the current prompt");
    if (!candidate.is_null())
        require(candidate.at("hint")==milk_hint &&
            candidate.at("condition")=="主人私聊中随口分享日常小烦恼，希望自然接话" &&
            wire.find("本轮明确请求优先")!=std::string::npos,"semantic recall lost the conditional scope or current-request priority");
    std::cout<<Json{{"group",enabled ? "B" : "A"},{"case",row.name},{"reference",reference},
        {"candidateFirst",candidate.is_null() ? Json(nullptr) : Json(1)},{"firstPacketCandidate",candidate},
        {"encodeCalls",encoded_inputs.size()},{"requestCount",requests.size()},{"firstBytes",wire.size()}}.dump(-1,' ',true)<<'\n';

    if (enabled && row.name=="paraphrase") {
        replay.host.main_messages()->change_memory(reference,std::nullopt);
        chat_withheld(replay.send("semantic-revoked",row.prompt,{say(row.answer)}).front(),reference);
        require(encoded_inputs==expected_inputs,"semantic fallback encoded without an eligible scope");
    }
    verify_trace(replay);
}
}

// 按独立进程阶段回放项目与普通私聊，项目崩溃样例沿原档案恢复。
void tone_replay(const fs::path& root,std::string_view phase) {
    if (phase=="recent-seed") { Replay replay(root/"seed"); return; }
    if (phase=="recent-A" || phase=="recent-B") { recent_dialogue_pressure(root/std::string(phase.substr(7)),phase=="recent-B"); return; }
    if (phase=="recent-recover-A" || phase=="recent-recover-B") { recent_dialogue_recover(root/std::string(phase.substr(15)),phase.ends_with('B')); return; }
    if (phase=="recent-revoked") { recent_dialogue_recover(root/"revoked",true,true); return; }
    if (phase.starts_with("semantic-")) {semantic_compare(root,phase.substr(9)); return;}
    if (phase=="chat-seed") {chat_seed(root); return;}
    if (phase=="chat-A" || phase=="chat-B") {chat_compare(root/std::string(phase.substr(5)),phase=="chat-B"); return;}
    if (phase=="chat-guards") {chat_guards(root); return;}
    if (phase=="chat-preflight") {chat_preflight(root); return;}
    if (phase=="seed") {seed(root); return;}
    if (phase=="A" || phase=="B") {compare(root/std::string(phase),phase=="B"); return;}
    if (phase=="guards") {guards(root); return;}
    require(phase=="crash" || phase=="recover","unknown tone replay phase");
    Replay replay(root/"crash",phase);
    if (phase=="crash") {
        replay.interrupt=true;
        replay.send("interrupted","这句收一点",{note("这句收一点","绝妙开局"),say()});
        throw std::runtime_error("did not interrupt pending correction");
    }
    const auto reference=replay.reference("interrupted");
    shallow(replay.send("recovered","「朱砂项目」的进度如何？",{say()}).front(),reference);
    const auto audit=replay.send("recovered-audit","核对刚才的纠正原话。",{lookup(reference),say()});
    const auto restored=sample(audit.back(),"memory.tone_original");
    require(restored.at("quote")=="这句收一点" && restored.at("reason")==reason,"interruption lost durable annotation");
    verify_trace(replay);
    std::cout<<"Interrupted draft: restored "<<reference<<" in first request; exact source and reason in second audit request\n";
}
}
