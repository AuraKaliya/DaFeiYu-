#include "application.hpp"
#include "main_messages.hpp"
#include <windows.h>
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
    app::Application application({},data/"empty",data,model::Backend{[](const auto&,const auto&) {
        return model::Response{{model::Role::assistant,"相同的回答"}};
    }});
    application.bind_messages(db,instance,nullptr);
    auto& channel = *application.main_messages();
    channel.terminal("first","普通原话");
    const auto status = application.conversation().status().value();
    const agent::RunId run{status.session,status.turn};
    const auto send = [&](const std::string& key, const char* route) {
        const auto envelope = app::user_envelope(route,key,instance,run);
        const auto original = db->put("desktop",key,envelope,{{"state","pending"}});
        return channel.deliver(original.message.id,envelope);
    };
    if (send("本轮补充","append").at("state")!="waiting" || send("下一轮","enqueue").at("state")!="queued")
        throw std::runtime_error("incorrect input admission");
    const auto removed = send("移除","enqueue");
    if (!application.remove_queued_dialogue(std::stoull(removed.at("nativeMessage").get<std::string>()))) throw std::runtime_error("remove failed");
    drain(application);
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
}
// 使用独立目录与固定模型响应验收应用的消息实效。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=2) return 2;
        verify(std::filesystem::path(argv[1])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64()));
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
