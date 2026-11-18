#include "inspect_work.hpp"
#include <stdexcept>

namespace dfy::mc {
// 构造时仅校验输入，实际接纳由第一次原生轮询执行。
InspectWork::InspectWork(const BridgeConfig& config, std::string id, std::string arguments)
    : config_(config), id_(std::move(id)), arguments_(std::move(arguments)), deadline_(std::chrono::steady_clock::now() + std::chrono::seconds(35)) {
    static_cast<void>(inspect_request("inspect.begin", id_, config_.key, arguments_));
}

// 每轮最多处理一次非等待本机收发，桥接异常后尝试停止同一动作。
// 步骤：处理取消和期限 → 提交或查询原命令 → 校验退出或安排下一次读回。
std::optional<InspectReply> InspectWork::poll(bool cancelled) {
    const auto now = std::chrono::steady_clock::now();
    stopping_ = stopping_ || cancelled || now >= deadline_;
    if (stopping_ && !attempted_) return InspectReply{true, false, false, {}};
    if (now >= deadline_ + std::chrono::seconds(4)) throw std::runtime_error("MC: game exit unconfirmed");
    if (!pipe_ && !stopping_ && now < next_) return {};
    try {
        if (!pipe_) {
            if (stopping_) phase_ = Phase::stop;
            const char* method = phase_ == Phase::begin ? "inspect.begin" : phase_ == Phase::stop ? "inspect.stop" : "inspect.read";
            pipe_ = std::make_unique<PipeExchange>(config_.channel, inspect_request(method, id_, config_.key, arguments_));
        }
        if (phase_ == Phase::begin) attempted_ = true;
        auto wire = pipe_->poll();
        if (!wire) return {};
        auto value = inspect_reply(*wire, id_, arguments_);
        pipe_.reset();
        if (value.exited) return value;
        phase_ = Phase::read;
        next_ = now + std::chrono::milliseconds(80);
    } catch (...) {
        pipe_.reset();
        if (!attempted_ || phase_ == Phase::stop) throw;
        stopping_ = true;
    }
    return {};
}
}
