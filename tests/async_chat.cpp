#include "async_chat.hpp"
#include <windows.h>
#include <sstream>
#include <iostream>
#include <thread>
namespace {
using namespace dfy::app;
using namespace dfy::runtime;
// 在异步终端命令、唯一输出或退出边界违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 创建独立测试事件通道，观察消费者不作为问答依赖。
std::shared_ptr<dfy::events::BroadcastChannel> channel() {
    return std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(16,64),1,8);
}
struct Release {
    std::promise<void>& promise; bool released = false;
    // 即使断言失败也结束受控后台阻塞。
    ~Release() { if (!released) promise.set_value(); }
    // 一次解除测试模型等待。
    void open() { promise.set_value(); released = true; }
};
// 验证等待期间管理和取消仍可使用，普通问答拒绝且不会丢失原回合。
// 步骤：阻塞真实后台模型 → 执行本地管理及取消 → 观察唯一取消输出 → 实际退出后结束输入。
void commands() {
    std::promise<void> release, entered; const auto gate = release.get_future().share(); auto started = entered.get_future();
    dfy::agent::AsyncSession session({},channel(),2,
    // 保持后台模型未返回，验证终端仍可独立处理管理和取消。
    // 步骤：报告已进入 → 等待测试放行 → 返回迟到文本。
    [&](const auto&,const auto&) { entered.set_value(); gate.wait(); return std::string("late answer"); });
    Release cleanup{release}; std::ostringstream output, errors; unsigned managed = 0;
    ChatController chat(session,output,errors,[&](std::string_view text) { if (text == "/plugins list") { ++managed; return true; } return false; });
    chat.line("hello"); started.wait(); chat.line("another"); chat.line("/plugins list"); chat.line("/cancel"); chat.advance();
    check(managed == 1 && !errors.str().empty() && output.str().find("Request cancelled") != std::string::npos && session.busy(),
        "model wait blocked management or cancel lost original turn");
    const auto once = output.str(); chat.advance(); check(output.str() == once,"cancel result printed twice");
    cleanup.open(); chat.finish_input();
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!chat.done() && std::chrono::steady_clock::now() < deadline) { chat.advance(); std::this_thread::yield(); }
    check(chat.done() && !session.message_count() && output.str().find("late answer") == std::string::npos,"cancelled model committed or prevented shutdown");
}
// 验证实际终端循环在 EOF 后保留当前回答，同时继续推进宿主工作。
// 步骤：写入真实管道并关闭写端 → 运行异步终端 → 检查回答与宿主推进 → 归还借用句柄。
void input_loop() {
    HANDLE read = nullptr, write = nullptr; check(CreatePipe(&read,&write,nullptr,0),"chat pipe failed");
    DWORD count = 0; check(WriteFile(write,"hello\r\n",7,&count,nullptr) && count == 7,"chat pipe write failed"); CloseHandle(write);
    {
        dfy::platform::LineInput input(read,2,64);
        dfy::agent::AsyncSession session({},channel(),2,[](const auto&,const auto& messages) { return "reply:"+messages.back().content; });
        std::ostringstream output, errors; unsigned pumped = 0;
        check(async_chat(session,input,output,errors,[&] { ++pumped; }) == 0 && output.str().find("reply:hello") != std::string::npos &&
            errors.str().empty() && pumped && session.message_count() == 2 && session.closed() && input.exited(),"EOF cancelled accepted answer or blocked host pump");
    }
    CloseHandle(read);
}
struct SyncedBuffer : std::stringbuf {
    bool command_visible = false;
    // 记录本地命令内容是否已经同步到输出端，区分写入内存与可观察回执。
    int sync() override { command_visible |= str().find("local result") != std::string::npos; return std::stringbuf::sync(); }
};
// 验证命令回执在下一次输入之前刷新，不依赖后续问答或进程退出。
// 步骤：管道只提交一个本地命令 → 等待输出同步 → 再发送退出 → 核对无需模型调用。
void command_flush() {
    HANDLE read = nullptr, write = nullptr; check(CreatePipe(&read,&write,nullptr,0),"flush pipe failed");
    DWORD count = 0; check(WriteFile(write,"/local\n",7,&count,nullptr) && count == 7,"flush command write failed");
    try {
        dfy::platform::LineInput input(read,1,64);
        dfy::agent::AsyncSession session({},channel(),1,[](const auto&,const auto&) -> std::string { throw std::runtime_error("local command reached model"); });
        SyncedBuffer buffer; std::ostream output(&buffer); std::ostringstream errors; bool sent_exit = false;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
        check(async_chat(session,input,output,errors,
        // 只有本地回执真正刷新后才继续输入，超时明确暴露相互等待。
        // 步骤：核对截止 → 观察回执同步 → 一次发送退出命令。
        [&] {
            check(std::chrono::steady_clock::now() < deadline,"local command output remained buffered");
            if (buffer.command_visible && !sent_exit) {
                DWORD written = 0; check(WriteFile(write,"/exit\n",6,&written,nullptr) && written == 6,"flush exit write failed"); sent_exit = true;
            }
        },[&](std::string_view command) { if (command != "/local") return false; output << "local result\n"; return true; }) == 0 &&
            sent_exit && errors.str().empty(),"local output flush did not allow next input");
    } catch (...) { CloseHandle(write); CloseHandle(read); throw; }
    CloseHandle(write); CloseHandle(read);
}
// 验证通知背压对成功、失败、取消及清空均独立报告，重复推进不重复提示。
// 步骤：按结果类别准备拥堵队列 → 接纳并推进问答 → 核对唯一通知提示 → 清空并检查独立回执。
void notification_warnings() {
    for (unsigned outcome = 0; outcome < 3; ++outcome) {
        auto events = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,1);
        auto observer = events->subscribe();
        std::promise<void> release, entered; auto gate = release.get_future().share(); auto started = entered.get_future();
        dfy::agent::AsyncSession session({},events,1,
        // 保持模型可控，分别产生返回、抛错或被取消后迟到返回。
        // 步骤：报告进入 → 等待放行 → 按测试类别返回或抛错。
        [&](const auto&,const auto&) -> std::string {
            entered.set_value(); gate.wait();
            if (outcome == 1) throw std::runtime_error("controlled failure");
            return "ok";
        });
        Release cleanup{release}; std::ostringstream output, errors; ChatController chat(session,output,errors);
        if (outcome) session.clear();
        chat.line("private prompt"); started.wait();
        check(outcome ? errors.str().find("turn_started notification not accepted") != std::string::npos : errors.str().empty(),
            "start notification failure was hidden or accepted notification warned");
        if (outcome == 2) { chat.line("/cancel"); chat.advance(); }
        cleanup.open();
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while (session.busy() && std::chrono::steady_clock::now() < deadline) { chat.advance(); std::this_thread::yield(); }
        const auto kind = outcome == 0 ? "turn_completed" : outcome == 1 ? "turn_failed" : "turn_cancelled";
        const auto warning = std::string(kind)+" notification not accepted"; const auto once = errors.str();
        check(!session.busy() && once.find(warning) != std::string::npos && once.find(warning) == once.rfind(warning) &&
            once.find("private prompt") == std::string::npos && once.find("capacity exhausted") != std::string::npos,
            "terminal notification warning was missing, duplicated or contained prompt");
        chat.advance(); check(errors.str() == once,"notification warning repeated after exit");
        check(outcome == 0 ? session.message_count() == 2 && output.str().find("ok") != std::string::npos : !session.message_count(),
            "observation warning changed question result");
        output.str(""); chat.line("/status");
        check(output.str().find("finished=capacity exhausted") != std::string::npos && errors.str() == once &&
            (outcome != 0 || output.str().find("Question=succeeded") != std::string::npos),"status lost notification failure or repeated warning");

        chat.line("/clear");
        check(errors.str().find("session_cleared notification not accepted") != std::string::npos &&
            output.str().find("Context cleared") != std::string::npos && !session.message_count(),"clear receipt was hidden or rolled back clear");
    }
}
// 验证本地诊断可查看取消前后状态，最近快照稳定且不阻止下一轮或泄露内容。
// 步骤：查询空会话及运行中 → 取消后分别检查结算和退出 → 保留快照并清空 → 连续验证两类失败。
void status_queries() {
    auto events = channel(); auto observer = events->subscribe();
    std::promise<void> release, entered; auto gate = release.get_future().share(); auto started = entered.get_future(); unsigned calls = 0;
    dfy::agent::AsyncSession session({},events,1,
    // 用一轮受控调用和两类失败覆盖诊断，不向真实服务发送任何请求。
    // 步骤：计数调用 → 按轮次选择阻塞返回、抛错或超过历史容量的文本。
    [&](const auto&,const auto&) -> std::string {
        ++calls;
        if (calls == 1) { entered.set_value(); gate.wait(); return "private answer"; }
        if (calls == 2) throw std::runtime_error("private exception");
        return std::string(1024*1024,'x');
    });
    Release cleanup{release}; std::ostringstream output, errors; ChatController chat(session,output,errors);
    chat.line("/status"); check(!calls && output.str().find("No accepted question") != std::string::npos,"empty status invoked model");
    chat.line("private prompt"); started.wait(); output.str(""); chat.line("/status");
    check(calls == 1 && output.str().find("Question=pending") != std::string::npos &&
        output.str().find("Model call=running, exit=waiting") != std::string::npos &&
        output.str().find("finished=not emitted") != std::string::npos && output.str().find("private") == std::string::npos,
        "live status advanced model, invented completion or leaked content");

    chat.line("/cancel"); chat.advance(); output.str(""); chat.line("/status");
    check(output.str().find("Question=cancelled") != std::string::npos && output.str().find("Cancellation=user") != std::string::npos &&
        output.str().find("Model call=running, exit=waiting") != std::string::npos,"cancel status hid actual exit wait");
    cleanup.open();
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (session.busy() && std::chrono::steady_clock::now() < deadline) { chat.advance(); std::this_thread::yield(); }
    check(!session.busy(),"status test model did not exit");
    output.str(""); chat.line("/status"); const auto final = output.str();
    check(final.find("Question=cancelled") != std::string::npos && final.find("Model call=returned, exit=confirmed") != std::string::npos &&
        final.find("(final)") != std::string::npos && final.find("accepted (queued only); event=") != std::string::npos &&
        final.find("private") == std::string::npos,"latest status lost late-return facts or exposed response");
    chat.line("/clear"); output.str(""); chat.line("/status");
    check(output.str() == final,"clear or repeated status changed final diagnostic snapshot");

    for (const auto stage : {"model","history commit"}) {
        chat.line("another private prompt");
        const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while (session.busy() && std::chrono::steady_clock::now() < until) { chat.advance(); std::this_thread::yield(); }
        check(!session.busy(),"retained diagnostic prevented next question exit");
        output.str(""); errors.str(""); chat.line("/status");
        check(output.str().find(std::string("Question=failed, failure stage=")+stage) != std::string::npos &&
            output.str().find("private") == std::string::npos && errors.str().empty(),"failure status lost stage or exposed exception");
    }
    check(calls == 3 && !session.message_count(),"status queries changed history or caused extra model calls");
}
}
// 验收终端本地命令、取消和真实输入循环，模型只使用离线后台替身。
// 步骤：验证等待期间命令 → 验证 EOF 与输出刷新 → 验证观察失败与回合诊断 → 报告结果。
int main() {
    try { commands(); input_loop(); command_flush(); notification_warnings(); status_queries(); std::cout << "Async chat checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
