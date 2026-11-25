#include "pipe_exchange.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace dfy::mc {
// 只接受本机固定命名空间内的通道短名。
bool valid_channel(std::string_view channel) noexcept {
    return !channel.empty() && channel.size() <= 48 && std::all_of(channel.begin(), channel.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    });
}

// 创建客户端连接，进入非等待模式后才允许工具工作持有它。
// 步骤：校验名称与帧长 → 打开本机服务端 → 开启非等待收发。
PipeExchange::PipeExchange(std::string_view channel, std::string request, std::chrono::steady_clock::duration timeout)
    : handle_(INVALID_HANDLE_VALUE), request_(std::move(request)), deadline_(std::chrono::steady_clock::now() + timeout) {
    // 采掘可携带有界的原候选集合，每项至多包含十六个保护格。
    if (!valid_channel(channel) || request_.empty() || request_.size() >= 128*1024 ||
        request_.find_first_of("\r\n") != std::string::npos) throw std::invalid_argument("MC bridge: invalid channel or request");
    request_ += '\n';
    const std::wstring name = std::wstring(LR"(\\.\pipe\DaFeiYu.MC.)") + std::wstring(channel.begin(), channel.end());
    handle_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) throw PipeTransportError("MC bridge: unavailable");

    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr)) {
        CloseHandle(handle_);
        throw PipeTransportError("MC bridge: nonblocking mode unavailable");
    }
}

// 在当前宿主线程归还管道，不等待对端关闭。
PipeExchange::~PipeExchange() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }

// 每轮至多写一次、读一次，所有长度以 UTF-8 字节计。
// 步骤：核对期限 → 推进尚未发送的字节 → 累积读数并交付唯一完整行。
std::optional<std::string> PipeExchange::poll() {
    if (handle_ == INVALID_HANDLE_VALUE) throw std::logic_error("MC bridge: response already consumed");
    if (std::chrono::steady_clock::now() >= deadline_) throw PipeTransportError("MC bridge: timed out");
    if (sent_ < request_.size()) {
        DWORD written = 0;
        if (!WriteFile(handle_, request_.data() + sent_, static_cast<DWORD>(request_.size() - sent_), &written, nullptr))
            throw PipeTransportError("MC bridge: write failed");
        sent_ += written;
        if (sent_ < request_.size()) return {};
    }

    std::array<char, 4096> buffer;
    DWORD size = 0;
    if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &size, nullptr)) {
        if (GetLastError() == ERROR_NO_DATA) return {};
        throw PipeTransportError("MC bridge: disconnected");
    }
    if (response_.size() + size > 16384) throw std::runtime_error("MC bridge: oversized response");
    response_.append(buffer.data(), size);
    const auto end = response_.find('\n');
    if (end == std::string::npos) return {};
    if (!end || end + 1 != response_.size()) throw std::runtime_error("MC bridge: invalid frame");
    response_.resize(end);
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
    return std::move(response_);
}
}
