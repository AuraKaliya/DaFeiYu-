#include <dafeiyu/line_input.hpp>
#include <windows.h>
#include <chrono>
#include <iostream>
#include <thread>
namespace {
using namespace dfy::platform;
// 在输入顺序、容量或实际线程回收违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Pipe {
    HANDLE read = nullptr, write = nullptr;
    // 创建真实本地匿名管道，测试不读取用户终端。
    Pipe() { check(CreatePipe(&read,&write,nullptr,0) != 0,"input test pipe failed"); }
    // 在读取线程退出后归还测试管道句柄。
    ~Pipe() { if (read) CloseHandle(read); if (write) CloseHandle(write); }
    // 写入完整小块测试数据，失败不伪装为成功输入。
    void send(std::string_view text) { DWORD count = 0; check(WriteFile(write,text.data(),static_cast<DWORD>(text.size()),&count,nullptr) && count == text.size(),"pipe write failed"); }
    // 关闭写端制造真实 EOF，允许重复调用。
    void finish() { if (write) { CloseHandle(write); write = nullptr; } }
};
// 在有界时间内等待一项真实读取结果，宿主不会阻塞在 getline。
InputRead next(LineInput& input) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do { auto result = input.poll(); if (result.state != InputState::empty) return result; std::this_thread::yield(); }
    while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("input did not deliver in time");
}
// 验证跨块中文、CRLF、队列背压和 EOF 前末行，超限错误独立返回一次。
void lines() {
    Pipe pipe; LineInput input(pipe.read,1,8);
    pipe.send("中文\r"); pipe.send("\n12345678\r\nlast"); pipe.finish();
    check(next(input).line == "中文" && next(input).line == "12345678" && next(input).line == "last","input split or reordered UTF-8 lines");
    check(next(input).state == InputState::end && input.exited(),"input EOF preceded queued data or actual exit");
    Pipe excessive; LineInput limited(excessive.read,1,4); excessive.send("12345\n");
    const auto error = next(limited);
    check(error.state == InputState::failed && error.error == ERROR_BUFFER_OVERFLOW && next(limited).state == InputState::end,"overlong line was truncated or error repeated");
}
// 验证在读调用前后的取消竞争，以及满队列条件等待可以退出。
// 步骤：反复即时停止空管道 → 逐次确认线程回收 → 填满队列后停止并检查结束。
void cancellation() {
    for (int i = 0; i < 12; ++i) {
        Pipe pipe; LineInput input(pipe.read,1,16); input.close();
        check(next(input).state == InputState::end && input.exited(),"read cancellation race left live thread");
    }
    Pipe pipe; LineInput input(pipe.read,1,16); pipe.send("first\nsecond\nthird\n");
    check(next(input).line == "first","full queue setup failed"); input.close();
    check(next(input).state == InputState::end && input.exited(),"full queue cancellation remained blocked");
    DWORD flags = 0; check(GetHandleInformation(pipe.read,&flags),"input closed borrowed pipe handle");
}
}
// 验收真实 Windows 管道上的有界后台读取及可重复推进的取消。
// 步骤：验证行边界和错误 → 验证取消竞争与回收 → 报告结果。
int main() {
    try { lines(); cancellation(); std::cout << "Line input checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
