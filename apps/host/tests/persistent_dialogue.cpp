#include "application.hpp"
#include "main_messages.hpp"
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
        seen.emplace_back(input.messages.back().content,input.messages.back().event_utc_ms);
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
            const auto& last = request.at("messages").back();
            if (last.at("content")=="提前追问") {
                const auto& tools = request.at("tools");
                offered = tools.size()==1 && tools[0].at("name")=="main_reply" &&
                    Json::parse(tools[0].at("parameters").get<std::string>()).at("required")==Json::array({"text"}) &&
                    request.at("messages")[0].at("content").get<std::string>().find("main_reply")!=std::string::npos;
            }
            if (last.at("role")=="tool" && last.at("toolCallId")=="speak") {
                const auto& body = last.at("content").get_ref<const std::string&>();
                const auto receipt = Json::parse(body.substr(body.find('{')));
                paired = receipt.at("state")=="delivered" && receipt.at("message")==std::to_string(explicit_reply.id) &&
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
        const auto& last = request.messages.back();
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
}
// 使用独立目录与固定模型响应验收应用的消息实效。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=2) return 2;
        const auto root = std::filesystem::path(argv[1])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        verify(root/"long-history");
        verify_output(root/"output");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
