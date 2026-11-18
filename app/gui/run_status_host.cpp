#include "run_status_host.hpp"
#include <cstdio>

namespace dfy::ui {
namespace {
constexpr auto interval = std::chrono::milliseconds(5);

// 先等待会话退出，再推进原插件停止及回收边界。
// 步骤：关闭会话接纳 → 等待实际退出 → 有界推进插件关闭并检查结果。
void shutdown(app::Application& application) {
    auto& conversation = application.conversation();
    conversation.close();
    while (!conversation.closed()) {
        application.advance_plugins();
        conversation.advance();
        std::this_thread::sleep_for(interval);
    }

    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    auto result = application.close_plugins();
    while (result != plugins::ShutdownResult::closed) {
        if (result == plugins::ShutdownResult::failed || std::chrono::steady_clock::now() >= deadline ||
            application.advance_plugins() == runtime::RunResult::failed)
            throw std::runtime_error("UI: plugin shutdown incomplete");
        std::this_thread::sleep_for(interval);
        result = application.close_plugins();
    }
}
}

// 将创建和推进入口移交新业务线程，线程的寿命短于快照存储。
RunStatusHost::RunStatusHost(Open open, Drive drive)
    : worker_([this,open = std::move(open),drive = std::move(drive)](std::stop_token stop) { run(stop,open,drive); }) {}

// 返回脱离业务对象寿命的当前值。
std::optional<app::RunStatusSnapshot> RunStatusHost::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

// 只在业务线程拥有和使用应用服务。
// 步骤：创建应用 → 推进原工作并发布快照 → 有序关闭并报告退出。
void RunStatusHost::run(std::stop_token stop, const Open& open, const Drive& drive) noexcept {
    try {
        const auto application = open();
        while (!stop.stop_requested()) {
            if (drive) drive(*application);
            if (application->advance_plugins() == runtime::RunResult::failed) std::fputs("A plugin notification failed.\n",stderr);
            application->conversation().advance();

            const auto snapshot = application->run_status();
            { std::lock_guard lock(mutex_); snapshot_ = snapshot; }
            std::this_thread::sleep_for(interval);
        }
        shutdown(*application);
    } catch (const std::exception& error) { failed_ = true; std::fprintf(stderr,"UI application host: %s\n",error.what()); }
    catch (...) { failed_ = true; }
    exited_ = true;
}
}
