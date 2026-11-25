#pragma once
#include "../conversation.hpp"
#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

namespace dfy::app { class Application; }
namespace dfy::management {
// 原生终端在管道线程登记命令，只有本体业务线程可以操作应用。
class TerminalHost {
  public:
    // 在原客户端空间登记或查回同一份命令，返回值区分排队和业务回包。
    nlohmann::json exchange(const std::string& client, const nlohmann::json& request);
    // 归还客户端的回执位置，已接纳命令仍留在业务队列。
    void disconnect(const std::string& client);
    // 更新可呈现提问的终端并在应用线程交付一项命令。
    void advance(app::Application& application);
    // 在业务线程把已写盘的用户发言交给在线终端。
    void published(const std::string& text);
    // 交付终端命令的结算和诊断，主聊天正文由其渠道回执发布。
    void completed(const app::CompletedTurn& turn, bool persistent);
  private:
    struct Job { nlohmann::json request, reply; std::string client; bool interactive; std::uint64_t offered_ask; };
    struct Client {
        std::shared_ptr<Job> last;
        bool interactive = false;
        std::uint64_t offered_ask = 0;
        std::chrono::steady_clock::time_point seen;
    };
    struct Output { std::uint64_t cursor; std::string out, err, client; };
    // 将一条实际输出追加到有界窗口，可按原终端限定接收方。
    void append_output(std::string out, std::string err, std::string client = {});
    // 在原应用上执行终端命令、采样状态或确认已经打印的问题。
    nlohmann::json execute(app::Application& application, const Job& job);
    std::mutex mutex_;
    std::map<std::string,Client> clients_;
    std::deque<std::shared_ptr<Job>> pending_;
    std::deque<Output> outputs_;
    std::size_t output_bytes_ = 0;
    std::uint64_t cursor_ = 0;
    agent::RunId command_run_{};
    std::string command_client_;
};
}
