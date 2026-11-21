#pragma once
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dfy::mc {
// 标识可对同一工作再次查询的管道传输故障。
struct PipeTransportError : std::runtime_error { using std::runtime_error::runtime_error; };
// 判断配置是否仅含本地管道短名允许的 ASCII 字符。
bool valid_channel(std::string_view channel) noexcept;

// 一次本地管道收发，宿主每次推进只执行有界非等待 I/O。
class PipeExchange {
  public:
    // 打开本次独占客户端连接并预留单行请求。
    PipeExchange(std::string_view channel, std::string request,
        std::chrono::steady_clock::duration timeout = std::chrono::seconds(3));
    // 归还原连接，包括提前取消和尚未完成的只读请求。
    ~PipeExchange();
    // 禁止复制连接和发送进度。
    PipeExchange(const PipeExchange&) = delete;
    // 禁止覆盖未结束的连接。
    PipeExchange& operator=(const PipeExchange&) = delete;
    // 推进发送和读取，完整行返回一次，超时、断线和超限明确失败。
    std::optional<std::string> poll();
  private:
    void* handle_;
    std::string request_, response_;
    std::size_t sent_ = 0;
    std::chrono::steady_clock::time_point deadline_;
};
}
