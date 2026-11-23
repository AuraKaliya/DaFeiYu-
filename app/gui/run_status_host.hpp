#pragma once
#include "application.hpp"
#include "dialogue_channel.hpp"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace dfy::ui {
// 业务线程独占应用服务，主窗口只借用快照的值副本。
class RunStatusHost {
  public:
    using Open = std::function<std::unique_ptr<app::Application>()>;
    using Drive = std::function<void(app::Application&)>;
    // 在业务线程创建应用，按原顺序推进会话与插件。
    explicit RunStatusHost(Open open, Drive drive = {});
    // 只复制最近发布的快照，锁内不调用应用服务。
    std::optional<app::RunStatusSnapshot> snapshot() const;
    // 读取激活状态、最新显示快照及指定命令的接纳回执。
    DialogueView dialogue(std::uint64_t command = 0) const;
    // 同号同文复用回执；仅在可接纳时把新命令交给业务线程。
    DialogueReceipt submit_dialogue(std::uint64_t command, std::string text);
    // 请求业务线程开始有序停止，实际退出由调用方后续查询。
    void close() noexcept { worker_.request_stop(); }
    // 查询业务线程是否已经离开应用的推进和收尾阶段。
    bool exited() const noexcept { return exited_; }
    // 查询初始化、推进或退出是否出现宿主错误。
    bool failed() const noexcept { return failed_; }
  private:
    struct CommandRecord { DialogueReceipt receipt; std::string fingerprint; };
    struct DialogueCommand { std::uint64_t id; std::string text; };
    // 取出当前命令，在业务线程进入正式主会话并保存原回执。
    void advance_dialogue(app::Application& application);
    // 推进原应用服务、发布值快照并在拥有线程上完成关闭。
    void run(std::stop_token stop, const Open& open, const Drive& drive) noexcept;

    mutable std::mutex mutex_;
    std::optional<app::RunStatusSnapshot> snapshot_;
    std::shared_ptr<const app::ConversationDisplay> display_;
    std::deque<CommandRecord> commands_;
    std::optional<DialogueCommand> pending_;
    std::uint64_t latest_command_ = 0;
    bool can_submit_ = false;
    std::atomic_bool exited_ = false, failed_ = false;
    std::jthread worker_;
};
}
