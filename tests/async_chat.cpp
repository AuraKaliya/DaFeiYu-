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
    dfy::agent::AsyncSession session({},channel(),2,[&](const auto&,const auto&) { entered.set_value(); gate.wait(); return std::string("late answer"); });
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
}
// 验收终端本地命令、取消和真实输入循环，模型只使用离线后台替身。
// 步骤：验证等待期间命令 → 验证 EOF 与宿主推进 → 报告结果。
int main() {
    try { commands(); input_loop(); std::cout << "Async chat checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
