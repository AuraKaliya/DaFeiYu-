#pragma once
#include <dafeiyu/run_capture.hpp>
#include <dafeiyu/event_store.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json_fwd.hpp>

namespace dfy::app {
// 单一宿主线程写入的局内日志，与运行和原生操作的结算责任独立。
class RunLog final : public std::enable_shared_from_this<RunLog> {
  public:
    // 为一次宿主启动分配独立目录，历史记录继续保留。
    explicit RunLog(const std::filesystem::path& root);
    // 所有共享记录者退出后标记本次日志结束。
    ~RunLog();
    // 返回本次运行的实际日志文件。
    const std::filesystem::path& path() const noexcept { return path_; }
    // 把同一输出绑定到指定来源的 Agent 会话。
    agent::RunCaptureSinks sinks(std::string source);
    // 在宿主线程追加已核对的事件，记录失败独立于业务处理。
    void append(std::string_view kind, nlohmann::json fields) noexcept;
    // 记录本体文本信件的原地址、发送者、接纳序号及可选目标回合。
    void mail(std::string_view kind, runtime::DomainId mailbox, runtime::DomainId sender, events::EventId id,
        std::string_view category, std::string_view body, std::optional<agent::RunId> run = {}) noexcept;
  private:
    // 写入一次模型记录，供后续按原请求身份复盘。
    void model(std::string_view source, const agent::ModelCapture& value);
    // 写入候选、实际接纳及退出时的原工作证据。
    void tool(std::string_view source, const agent::ToolCapture& value);
    // 写入已经退出的运行，包括元数据与实际内部结算。
    void exit(std::string_view source, const agent::TurnRequest& request);

    std::filesystem::path path_;
    std::ofstream output_;
    const std::chrono::steady_clock::time_point origin_ = std::chrono::steady_clock::now();
    std::uint64_t sequence_ = 0;
    bool failed_ = false;
};
}
