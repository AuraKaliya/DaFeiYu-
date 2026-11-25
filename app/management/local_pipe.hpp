#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <cstdint>

namespace dfy::app { class SoftwareLog; }
namespace dfy::management {
// 在业务开始前独占实际数据目录，进程退出后由操作系统归还写入资格。
class ProfileLease {
  public:
    // 通过无共享方式打开同一目录中的固定锁文件，碰撞时不创建 Application。
    explicit ProfileLease(const std::filesystem::path& profile, app::SoftwareLog* log = nullptr);
    // 归还持有的系统句柄，遗留的空文件不代表本体在运行。
    ~ProfileLease();
  private:
    struct State;
    std::unique_ptr<State> state_;
};

// 一个用户数据目录对应一个本地服务端，连接只允许当前 Windows 用户使用。
class LocalPipe {
  public:
    using Handler = std::function<std::string(const std::string&)>;
    // 以目录的文件身份命名端点，在接纳业务前取得唯一管道实例。
    explicit LocalPipe(const std::filesystem::path& profile);
    // 关闭操作系统端点，之后同一目录可以重新取得所有权。
    ~LocalPipe();
    // 在给定期限内等候一位客户端，接收完整请求后处理并交付有界回复。
    bool serve_one(std::chrono::milliseconds wait, const Handler& handle);
  private:
    struct State;
    std::unique_ptr<State> state_;
};

using PeerReply = std::function<void(void* process, std::uint32_t pid, std::string_view reply)>;
// 核对当前用户的管道对端，回执确认前可借用原进程对象保存退出观察权。
std::string local_request(const std::filesystem::path& profile, const std::string& request,
    std::chrono::milliseconds timeout = std::chrono::seconds(5), const PeerReply& before_ack = {});
}
