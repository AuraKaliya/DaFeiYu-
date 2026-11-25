#pragma once
#include "resident_watch.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>

namespace dfy::app { class SoftwareLog; }
namespace dfy::management {
// 在工作线程连接本体，回调只在观察者自己的推进线程执行。
class Client {
  public:
    using Reply = std::function<void(const nlohmann::json&)>;
    using Launch = std::function<void()>;
    using Logs = std::function<nlohmann::json(bool open)>;
    // 连接原宿主，只有端点缺失时才调用启动入口一次。
    Client(std::filesystem::path profile, std::string definition, Launch start, Logs logs = {}, app::SoftwareLog* software = nullptr);
    // 在销毁前唤醒等待线程并回收本客户端。
    ~Client();
    // 在有界队列中登记一项请求，交付结果由后续推进处理。
    bool post(const nlohmann::json& request, Reply reply);
    // 在调用方线程交付已经完成的值副本。
    void advance();
    // 非阻塞读取原进程的实际退出码与已核实的显式退出意图。
    ResidentObservation observe() const noexcept;
    // 停止新接纳，跳过尚未执行的读取并交付已排队的写命令。
    void close() noexcept;
    // 查询连接线程是否已完成分离。
    bool exited() const noexcept { return exited_; }
  private:
    struct Entry { nlohmann::json message; Reply deliver; std::size_t bytes; };
    // 握手后按顺序交换请求，关闭时释放自己的客户端身份。
    void run(std::stop_token stop, const std::filesystem::path& profile, const std::string& definition, const Launch& start, const Logs& logs);
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Entry> pending_, completed_;
    std::size_t queued_bytes_ = 0;
    std::atomic_bool exited_ = false;
    app::SoftwareLog* software_;
    std::shared_ptr<ResidentWatch> watch_;
    std::atomic_bool stop_accepted_ = false;
    std::jthread thread_;
};

// 隐藏启动指定的后台程序，命令参数直接传递给 CreateProcessW。
void launch_host(const std::filesystem::path& program, const std::filesystem::path& config,
    const std::filesystem::path& packages, const std::filesystem::path& profile, const std::filesystem::path& trace = {});
struct ResidentConnection;
// 初次连接缺少端点时启动一次后台，按原配置有界重试握手。
ResidentConnection connect_or_launch(std::stop_token stop, const std::filesystem::path& profile, const std::string& definition,
    const Client::Launch& start, app::SoftwareLog* log);
}
