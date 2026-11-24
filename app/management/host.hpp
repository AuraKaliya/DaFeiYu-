#pragma once
#include "application.hpp"
#include "dialogue_channel.hpp"
#include "room_location.hpp"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <map>
#include <thread>

namespace dfy::management {
// 业务线程独占应用服务，观察端可独立读取快照或离开。
class Host {
  public:
    using Open = std::function<std::unique_ptr<app::Application>()>;
    using Drive = std::function<void(app::Application&)>;
    // 在业务线程创建应用，按原顺序推进会话与插件。
    explicit Host(Open open, Drive drive = {});
    // 只复制最近发布的快照，锁内不调用应用服务。
    std::optional<app::RunStatusSnapshot> snapshot() const;
    // 复制业务线程已经核实的房间与身体位置，无需唤醒主会话。
    std::optional<RoomLocation> room_location() const;
    // 为新观察客户端签发独立命令空间，正常页面刷新沿用原空间。
    std::string open_client();
    // 核对远端观察者身份并更新活跃时间，不读取正文。
    bool visit_client(const std::string& client);
    // 撤销观察客户端的提交身份，已接纳的工作继续由宿主持有。
    void close_client(const std::string& client);
    // 读取共享对话与指定客户端自己的命令水位和回执。
    DialogueView dialogue(std::uint64_t command = 0, const std::string& client = {}) const;
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
    struct Client {
        std::uint64_t latest = 0;
        std::chrono::steady_clock::time_point seen = std::chrono::steady_clock::now();
        std::deque<CommandRecord> commands;
    };
    // 在业务线程执行指定槽位中的命令，并将原回执保留到发布结果。
    void advance_dialogue(app::Application& application, std::optional<DialogueCommand>& slot);
    // 推进原应用服务、发布值快照并在拥有线程上完成关闭。
    void run(std::stop_token stop, const Open& open, const Drive& drive) noexcept;

    mutable std::mutex mutex_;
    std::optional<app::RunStatusSnapshot> snapshot_;
    std::optional<RoomLocation> room_location_;
    std::shared_ptr<const app::ConversationDisplay> display_;
    std::optional<DialogueCommand> pending_, stop_command_;
    mutable std::map<std::string,Client> clients_{{"",{}}};
    bool can_submit_ = false;
    std::atomic_bool exited_ = false, failed_ = false;
    std::jthread worker_;
};
}
