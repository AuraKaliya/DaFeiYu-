#include "game_work.hpp"
#include <stdexcept>

namespace dfy::mc {
// 构造时仅校验输入，实际接纳由第一次原生轮询执行。
GameWork::GameWork(GameAction action, const BridgeConfig& config, std::string id, std::string arguments)
    : config_(config), action_(action), id_(std::move(id)), arguments_(std::move(arguments)), deadline_(std::chrono::steady_clock::now() + std::chrono::seconds(35)) {
    if (action_ == GameAction::inspect) static_cast<void>(inspect_request("inspect.begin", id_, config_.key, arguments_));
    else static_cast<void>(mine_request("mine.begin", id_, config_.key, arguments_));
}

// 每轮最多处理一次非等待本机收发，桥接异常后尝试停止同一动作。
// 步骤：处理取消和期限 → 提交或查询原命令 → 校验退出或安排下一次读回。
std::optional<GameResult> GameWork::poll(bool cancelled) {
    const auto now = std::chrono::steady_clock::now();
    stopping_ = stopping_ || cancelled || now >= deadline_;
    if (stopping_ && !attempted_) return GameResult{false, GameEffect::none, {}};
    if (now >= deadline_ + std::chrono::seconds(4)) throw std::runtime_error("MC: game exit unconfirmed");
    if (!pipe_ && !stopping_ && now < next_) return {};
    try {
        if (!pipe_) {
            if (stopping_) phase_ = Phase::stop;
            const char* verb = phase_ == Phase::begin ? ".begin" : phase_ == Phase::stop ? ".stop" : ".read";
            const auto frame = action_ == GameAction::inspect ? inspect_request(std::string("inspect") + verb, id_, config_.key, arguments_)
                : mine_request(std::string("mine") + verb, id_, config_.key, arguments_);
            pipe_ = std::make_unique<PipeExchange>(config_.channel, frame);
        }
        if (phase_ == Phase::begin) attempted_ = true;
        auto wire = pipe_->poll();
        if (!wire) return {};
        pipe_.reset();
        if (action_ == GameAction::inspect) {
            auto value = inspect_reply(*wire, id_, arguments_);
            if (value.exited) return GameResult{value.arrived, value.moved ? GameEffect::observed : GameEffect::none, std::move(value.payload)};
        } else {
            auto value = mine_reply(*wire, id_, arguments_);
            if (value.exited) return GameResult{value.finished, value.started ? GameEffect::unknown : GameEffect::none, std::move(value.payload)};
        }
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
