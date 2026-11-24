#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace dfy::management {
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

// 连接当前用户拥有的端点，以单次有界交换读回一项操作的原始回复。
std::string local_request(const std::filesystem::path& profile, const std::string& request,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));
}
