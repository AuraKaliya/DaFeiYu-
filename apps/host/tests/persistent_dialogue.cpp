#include "../../../tests/decision_input.hpp"
#include "application.hpp"
#include "main_messages.hpp"
#include "trace_commands.hpp"
#include <algorithm>
#include <sstream>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace dfy;
using Json = nlohmann::json;
// 保持正式业务推进次序，直到原回合和等待队列都结束。
void drain(app::Application& application) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
    do {
        application.advance_plugins();
        application.conversation().advance();
        if (std::chrono::steady_clock::now()>deadline) throw std::runtime_error("pending fixture dialogue");
        std::this_thread::yield();
    } while (application.conversation().busy() || !application.conversation().display().queue.empty());
}
// 在关闭 Trace 的真实应用上验证长历史、同文异号、追加实收和队列去向。
// 步骤：提交并移除不同消息 → 等待实际带入与下一轮 → 超过显示保留窗口 → 清空上下文后核对独立历史。
void verify(const std::filesystem::path& data) {
    const std::string instance = "1-2-3";
    auto db = std::make_shared<app::MessageStore>(data);
    std::vector<std::pair<std::string,std::optional<std::int64_t>>> seen;
    app::Application application({},data/"empty",data,model::Backend{[&](const auto&,const model::Request& input) {
        seen.emplace_back(dfy::test::business_messages(input).back().content,dfy::test::business_messages(input).back().event_utc_ms);
        return model::Response{{model::Role::assistant,"相同的回答"}};
    }});
    application.bind_messages(db,instance,nullptr);
    auto& channel = *application.main_messages();
    channel.terminal("first","普通原话");
    const auto status = application.conversation().status().value();
    const agent::RunId run{status.session,status.turn};
    const auto send = [&](const std::string& key, const char* route) {
        const auto envelope = app::user_envelope(route,key,instance,run,key=="本轮补充" ? 123 : 456);
        const auto original = db->put("desktop",key,envelope,{{"state","pending"}});
        return channel.deliver(original.message.id,envelope,original.message.utc_ms);
    };
    if (send("本轮补充","append").at("state")!="waiting" || send("下一轮","enqueue").at("state")!="queued")
        throw std::runtime_error("incorrect input admission");
    const auto removed = send("移除","enqueue");
    if (!application.remove_queued_dialogue(std::stoull(removed.at("nativeMessage").get<std::string>()))) throw std::runtime_error("remove failed");
    drain(application);
    if (seen.size()!=3 || seen[0].second!=db->find("terminal","first")->utc_ms ||
        seen[1].first!="本轮补充" || seen[1].second!=123 || seen[2].first!="下一轮" || seen[2].second!=456)
        throw std::runtime_error("persistent dialogue changed source time before model input");
    const auto supplement = db->find("desktop","本轮补充").value(), queued = db->find("desktop","下一轮").value();
    if (supplement.receipt.at("state")!="submitted" || supplement.receipt.at("modelStep")!="2" || queued.receipt.at("state")!="accepted" ||
        queued.receipt.at("turn")==std::to_string(status.turn) || db->find("desktop","移除")->receipt.at("state")!="removed")
        throw std::runtime_error("actual supplement or queue effect missing");
    for (int i=0; i<17; ++i) {
        channel.terminal("repeat-"+std::to_string(i),"独立的同文消息");
        drain(application);
    }
    if (application.conversation().display().turns.size()!=16) throw std::runtime_error("fixture did not exceed display retention");
    application.conversation().clear();
    const auto page = db->page(true,0,db->watermark(),64);
    unsigned answers = 0;
    for (const auto& row : page.records) {
        if (row.source!="main") continue;
        if (row.envelope.at("text")!="相同的回答" || !row.envelope.contains("replyTo") || row.receipt.at("state")!="delivered")
            throw std::runtime_error("answer lost origin or actual delivery");
        ++answers;
    }
    if (answers!=19 || page.records.size()!=40 || !application.trace_file().empty()) throw std::runtime_error("durable history was truncated");
    application.post_room_event(app::RoomEvent::addressed,"书房","有人问该交给谁");
    drain(application);
    const auto proactive = db->page(true,0,db->watermark(),1).records.front();
    if (proactive.source!="main" || proactive.envelope.contains("replyTo") || proactive.id==page.records.front().id)
        throw std::runtime_error("addressed room could not reply without a window");
}

// 校验真实送模契约、显式发言回执和重复最终文本都绑定同一原回合。
// 步骤：按回合读取实际准备记录 → 定位工具配对和原投递 → 核对内部回合的范围。
void check_output_trace(const std::filesystem::path& path, const app::StoredMessage& explicit_reply) {
    std::ifstream input(path,std::ios::binary);
    bool offered = false, paired = false, delivered = false, reused = false, internal = false;
    for (std::string line; std::getline(input,line);) {
        const auto row = Json::parse(line);
        const auto& kind = row.at("kind");
        if (kind=="model.prepared") {
            const auto& request = row.at("input");
            const auto& messages = request.at("messages");
            const auto status = Json::parse(messages.back().at("content").get<std::string>());
            if (row.at("messageEvidence").back().at("source")!="host_status" || status.at("step")!=row.at("step") ||
                status.at("schema")!="dafeiyu.agent_status/1" || status.at("turn")!=row.at("run"))
                throw std::runtime_error("decision status not bound to the actual main request");
            const auto& last = messages.at(messages.size()-2);
            if (last.at("content")=="提前追问") {
                const auto& tools = request.at("tools");
                offered = tools.size()==2 && tools[0].at("name")=="main_reply" && tools[1].at("name")=="status_read" &&
                    status.at("items").empty() &&
                    Json::parse(tools[0].at("parameters").get<std::string>()).at("required")==Json::array({"text"}) &&
                    request.at("messages")[0].at("content").get<std::string>().find("main_reply")!=std::string::npos;
            }
            if (last.at("role")=="tool" && last.at("toolCallId")=="speak") {
                const auto& body = last.at("content").get_ref<const std::string&>();
                const auto receipt = Json::parse(body.substr(body.find('{')));
                const auto& items = status.at("items");
                paired = items.size()==1 && items[0].at("key")=="host.delivery" &&
                    items[0].at("value").at("delivery")==explicit_reply.submission &&
                    items[0].at("fact")==explicit_reply.id && receipt.at("state")=="delivered" && receipt.at("message")==std::to_string(explicit_reply.id) &&
                    receipt.at("delivery")==explicit_reply.submission;
            }
            if (last.at("content")=="内部结算") internal = request.at("tools").empty();
        }
        if (row.value("delivery",std::string{})!=explicit_reply.submission) continue;
        if (kind=="dialogue.reply_delivered") delivered = row.at("message")==std::to_string(explicit_reply.id) &&
            explicit_reply.envelope.at("turn")==std::to_string(row.at("run").get<std::uint64_t>());
        if (kind=="dialogue.reply_reused") reused = true;
    }
    if (!offered || !paired || !delivered || !reused || !internal) throw std::runtime_error("missing main output trace evidence");
}

// 通过共用循环核对提前追问、独立同文答复及内部结算的真实渠道效果。
// 步骤：显式发言后返回相同最终文本 → 发起独立问答及静默回合 → 比对消息库与 Trace。
void verify_output(const std::filesystem::path& data) {
    auto db = std::make_shared<app::MessageStore>(data);
    const std::string question = "这批材料要交给谁？";
    app::ApplicationRunConfig run;
    run.trace_directory = data/"trace-evidence";
    // 按实际请求给出固定问答与工具材料，让真实宿主决定每条输出的去向。
    // 步骤：识别首次发言或静默场景 → 返回内部回执或应交付用户的最终文本。
    app::Application application({},data/"empty",data,model::Backend{[&](const auto&,const model::Request& request) {
        const auto& last = dfy::test::business_messages(request).back();
        if (last.content=="提前追问") return model::Response{{model::Role::assistant,"",{{"speak","main_reply",Json{{"text",question}}.dump()}}},
            model::FinishReason::tool_calls};
        if (last.content=="静默") return model::Response{{model::Role::assistant," \t "}};
        if (last.content=="内部结算") return model::Response{{model::Role::assistant,"只交给上级的结果"}};
        return model::Response{{model::Role::assistant,question}};
    }},run);
    application.bind_messages(db,"fixed-instance",nullptr);
    auto& channel = *application.main_messages();
    for (const auto& prompt : {"提前追问","独立原话","静默"}) {
        channel.terminal(prompt,prompt);
        drain(application);
    }
    application.conversation().submit("内部结算");
    drain(application);
    const auto rows = db->page(true,0,db->watermark(),16).records;
    if (rows.size()!=5 || rows[1].source!="main" || rows[3].source!="main" || rows[1].envelope.at("text")!=question ||
        rows[3].envelope.at("text")!=question || rows[1].submission==rows[3].submission ||
        rows[1].envelope.at("replyTo")!=std::to_string(rows[2].id) || rows[3].envelope.at("replyTo")!=std::to_string(rows[4].id))
        throw std::runtime_error("explicit reply, independent final or internal result routed incorrectly");
    check_output_trace(application.trace_file(),rows[3]);
}

// 在正式主聊天中按原快照分别读取发言前后两版，验证同批配对及完整离线重放。
// 步骤：记录发言前快照 → 发言后冻结原实收 → 同批核对两版投递和计数 → 对照 SQLite 消息和重放。
void verify_version_query(const std::filesystem::path& data) {
    using namespace model;
    auto db = std::make_shared<app::MessageStore>(data);
    app::ApplicationRunConfig config;
    config.trace_directory = data/"evidence";
    std::string before, after, delivery;
    unsigned steps = 0;
    // 按实际输入兑现固定响应，同批读取的两条回执分别绑定原调用。
    // 步骤：请求发言 → 记录两版并发起读取 → 对照两条工具输出。
    app::Application application({},data/"empty",data,Backend{[&](const Config&,const Request& input) -> Response {
        const auto& last = input.messages.back();
        const auto note = Json::parse(last.content);
        const auto messages = dfy::test::business_messages(input);
        if (last.source!=MessageSource::host_status || note.at("step")!=++steps) throw std::runtime_error("unbound decision");
        if (steps==1) {
            if (!note.at("items").empty()) throw std::runtime_error("first decision invented a delivery");
            before = note.at("snapshot");
            return {{Role::assistant,"",{{"speech","main_reply",R"({"text":"先答复。"})"}}},FinishReason::tool_calls};
        }
        if (steps==2) {
            if (messages.back().tool_call_id!="speech") throw std::runtime_error("speech receipt lost its original call");
            after = note.at("snapshot");
            delivery = note.at("items").at(0).at("value").at("delivery");
            return {{Role::assistant,"",{{"earlier","status_read",Json{{"snapshot",before}}.dump()},
                {"later","status_read",Json{{"snapshot",after}}.dump()}}},FinishReason::tool_calls};
        }
        for (std::size_t i=0;i<2;++i) {
            const auto& result = messages[messages.size()-2+i];
            const auto read = Json::parse(result.content.substr(result.content.find('{')));
            const auto& status = read.at("status");
            const auto& items = status.at("items");
            const auto spoken = std::ranges::find_if(items,[](const auto& item) { return item.at("key")=="host.delivery"; });
            const auto counts = std::ranges::find_if(items,[](const auto& item) { return item.at("key")=="host.tool_counts"; });
            if (result.role!=Role::tool || result.tool_call_id!=(i ? "later" : "earlier") ||
                read.at("state")!="ready" || status.at("snapshot")!=(i ? after : before) ||
                (i ? spoken==items.end() || spoken->at("value").at("delivery")!=delivery : spoken!=items.end()) ||
                counts==items.end() || counts->at("value").at("turn").at("dispatched")!=i)
                throw std::runtime_error("status_read returned a different decision or crossed receipt pairs");
        }
        return {{Role::assistant,"已查回两版。"}};
    }},config);
    application.bind_messages(db,"versions",nullptr);
    application.main_messages()->terminal("ask","核对刚才的发言");
    const auto run = application.conversation().status().value();
    drain(application);
    const auto rows = db->page(true,0,db->watermark(),8).records;
    if (steps!=3 || rows.size()!=3 || rows[1].submission!=delivery || rows[1].envelope.at("text")!="先答复。" ||
        rows[0].envelope.at("text")!="已查回两版。") throw std::runtime_error("original channel delivery differs from the frozen state");
    std::ostringstream replay;
    app::replay_trace(application.trace_file(),{run.session,run.turn},replay);
    if (replay.str().find("3 model requests, 3 tool exits, succeeded")==std::string::npos)
        throw std::runtime_error("external decision inputs or read receipts diverged in replay");
}

// 用两个真实人格文件驱动相同的主聊天输入，逐段比较职责、授权、判断和输出契约。
// 步骤：分别加载两份风格 → 捕获正式请求及当轮工具 → 核对规则顺序及实际送达。
void verify_styles(const std::filesystem::path& root) {
    std::vector<std::string> headers;
    for (const std::string style : {"brief","warm"}) {
        const auto data = root/style;
        std::filesystem::create_directories(data);
        std::ofstream(data/"persona.json") << Json{{"id","style"},{"revision",1},{"instructions",style}}.dump();
        auto db = std::make_shared<app::MessageStore>(data);
        app::Application application({},data/"empty",data,model::Backend{[&](const auto&,const model::Request& input) {
            headers.push_back(input.messages.front().content);
            if (input.messages.front().source!=model::MessageSource::rules || input.tools.size()!=2 || input.tools[0].name!="main_reply" || input.tools[1].name!="status_read")
                throw std::runtime_error("style changed granted tools");
            return model::Response{{model::Role::assistant,"相同的回执"}};
        }});
        application.bind_messages(db,"style-test",nullptr);
        application.main_messages()->terminal("greeting","问候");
        drain(application);
        const auto rows = db->page(true,0,db->watermark(),4).records;
        if (rows.size()!=2 || rows.front().receipt.at("state")!="delivered" || rows.front().envelope.at("text")!="相同的回执")
            throw std::runtime_error("style changed actual delivery");
        auto& head = headers.back();
        std::size_t offset = 0;
        for (const auto label : {"【职责与信息来源 v1】","【授权与行动边界 v1】","【判断与结算 v1】","【输出与投递 v1】","【人格与风格 v1】"}) {
            const auto begin = head.find(label,offset);
            if (begin==head.npos) throw std::runtime_error("rule sections lost their stable order");
            offset = begin+std::string_view(label).size();
        }
        if (head.substr(offset)!="\n"+style) throw std::runtime_error("persona was not confined to the style section");
        head.resize(offset);
    }
    if (headers[0]!=headers[1]) throw std::runtime_error("persona altered non-style rules");
}
}
// 使用独立目录与固定模型响应验收应用的消息实效。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=2) return 2;
        const auto root = std::filesystem::path(argv[1])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        verify(root/"long-history");
        verify_output(root/"output");
        verify_styles(root/"styles");
        verify_version_query(root/"versions");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
