#include "pipe_exchange.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
// 在测试时钟的有界范围内推进一个真实管道请求。
std::string read(std::string_view channel, std::string request) {
    dfy::mc::PipeExchange exchange(channel, std::move(request), std::chrono::milliseconds(600));
    for (;;) {
        if (auto line = exchange.poll()) return std::move(*line);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}

// 通过独立 Node 管道服务验证分片、容量、断开与期限。
// 步骤：校验短名 → 读取分片成功回执 → 检查各类失败结束。
int main(int argc, char** argv) {
    try {
        if (argc != 2 || dfy::mc::valid_channel("../invalid")) throw std::runtime_error("channel boundary");
        if (read(argv[1], "parts") != std::string(5000, 'x')) throw std::runtime_error("partial data");
        for (const auto request : {"oversized", "disconnect", "silent"}) {
            bool failed = false;
            try { static_cast<void>(read(argv[1], request)); }
            catch (const std::runtime_error&) { failed = true; }
            if (!failed) throw std::runtime_error("invalid reply accepted");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
