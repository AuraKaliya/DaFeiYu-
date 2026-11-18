#pragma once
#include "application.hpp"
#include <atomic>
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
    // 请求业务线程开始有序停止，实际退出由调用方后续查询。
    void close() noexcept { worker_.request_stop(); }
    // 查询业务线程是否已经离开应用的推进和收尾阶段。
    bool exited() const noexcept { return exited_; }
    // 查询初始化、推进或退出是否出现宿主错误。
    bool failed() const noexcept { return failed_; }
  private:
    // 推进原应用服务、发布值快照并在拥有线程上完成关闭。
    void run(std::stop_token stop, const Open& open, const Drive& drive) noexcept;

    mutable std::mutex mutex_;
    std::optional<app::RunStatusSnapshot> snapshot_;
    std::atomic_bool exited_ = false, failed_ = false;
    std::jthread worker_;
};
}
