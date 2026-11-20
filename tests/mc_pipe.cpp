#include "pipe_exchange.hpp"
#include "partner_feed.hpp"
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

// 在实际独立管道上验证插件的首次水位和后续事实增量。
// 步骤：等待配对玩家 → 核对旧话不重放 → 消费后验证最新原话。
void verify_feed(std::string_view channel) {
    dfy::mc::PartnerFeed feed;
    const dfy::mc::BridgeConfig config{std::string(channel),std::string(64,'1')};
    // 推进到指定来源序号已处于可交付状态。
    const auto wait = [&](std::uint64_t cursor) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline) {
            feed.advance(config,1);
            if (feed.ready() && feed.cursor() == cursor) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("partner feed did not reach its expected cursor");
    };
    wait(2);
    if (feed.world().empty() || feed.owner().empty() || feed.pending().size() != 1 ||
        feed.pending().front().at("type") != "player.entered") throw std::runtime_error("partner baseline repeated old chat");
    feed.consumed();
    wait(3);
    if (feed.pending().size() != 1 || feed.pending().front().at("text") != "current speech" || feed.gap())
        throw std::runtime_error("partner feed lost the verified raw message");
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
        verify_feed(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
