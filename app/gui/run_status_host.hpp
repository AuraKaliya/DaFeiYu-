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
    // 核对命令编号、种类、目标和原话，复用同一命令的回执。
    DialogueReceipt submit_dialogue(DialogueCommand command);
    // 请求业务线程开始有序停止，实际退出由调用方后续查询。
    void close() noexcept { worker_.request_stop(); }
    // 查询业务线程是否已经离开应用的推进和收尾阶段。
    bool exited() const noexcept { return exited_; }
    // 查询初始化、推进或退出是否出现宿主错误。
    bool failed() const noexcept { return failed_; }
  private:
    struct CommandRecord { DialogueReceipt receipt; DialogueAction action; agent::RunId target; std::uint64_t message; std::string fingerprint; };
    // 在业务线程执行指定槽位中的命令，并将原回执保留到发布结果。
    void advance_dialogue(app::Application& application, std::optional<DialogueCommand>& slot);
    // 推进原应用服务、发布值快照并在拥有线程上完成关闭。
    void run(std::stop_token stop, const Open& open, const Drive& drive) noexcept;

    mutable std::mutex mutex_;
    std::optional<app::RunStatusSnapshot> snapshot_;
    std::shared_ptr<const app::ConversationDisplay> display_;
    std::deque<CommandRecord> commands_;
    std::optional<DialogueCommand> pending_, stop_command_;
    std::uint64_t latest_command_ = 0;
    bool can_submit_ = false;
    std::atomic_bool exited_ = false, failed_ = false;
    std::jthread worker_;
};
}
