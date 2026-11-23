#include "game_work.hpp"
#include <stdexcept>

namespace dfy::mc {
// 构造时仅校验输入，实际接纳由第一次原生轮询执行。
GameWork::GameWork(GameAction action, const BridgeConfig& config, std::string id, std::string arguments, std::string skill)
    : config_(config), action_(action), id_(std::move(id)), arguments_(std::move(arguments)), skill_(std::move(skill)),
      deadline_(skill_ == "follow" ? std::chrono::steady_clock::time_point::max() :
          std::chrono::steady_clock::now() + std::chrono::seconds(skill_ == "route" ? 180 : 35)) {
    if (action_ == GameAction::partner) static_cast<void>(partner_request("partner.begin", id_, config_, skill_, arguments_));
    else if (action_ == GameAction::inspect) static_cast<void>(inspect_request("inspect.begin", id_, config_.key, arguments_));
    else if (action_ == GameAction::mine) static_cast<void>(mine_request("mine.begin", id_, config_.key, arguments_));
    else static_cast<void>(return_request("return.begin", id_, config_.key, arguments_));
}

// 放弃本地旧读口，延长一次查询原动作退出的时间。
void GameWork::recheck_exit() noexcept {
    pipe_.reset();
    stop_deadline_ = {};
    next_ = {};
    phase_ = Phase::stop;
}

// 每轮最多处理一次非等待本机收发，桥接异常后尝试停止同一动作。
// 步骤：处理取消、期限和接纳条件 → 提交或查询原命令 → 校验退出或安排下一次读回。
std::optional<GameResult> GameWork::poll(bool cancelled, bool defer, bool allow_begin) {
    const auto now = std::chrono::steady_clock::now();
    stopping_ = stopping_ || cancelled || now >= deadline_;
    if (stopping_) {
        if (!attempted_) return GameResult{false, GameEffect::none, {}};
        if (stop_deadline_ == decltype(stop_deadline_){}) stop_deadline_ = now + std::chrono::seconds(4);
        if (now >= stop_deadline_) throw std::runtime_error("MC: game exit unconfirmed");
    }
    if (!allow_begin && !attempted_) return {};
    if (!pipe_ && now < next_) return {};
    try {
        if (!pipe_) {
            if (stopping_) phase_ = Phase::stop;
            const char* verb = phase_ == Phase::begin ? ".begin" : phase_ == Phase::stop ? ".stop" : ".read";
            const auto frame = action_ == GameAction::partner ? partner_request(std::string("partner") + verb, id_, config_, skill_, arguments_)
                : action_ == GameAction::inspect ? inspect_request(std::string("inspect") + verb, id_, config_.key, arguments_)
                : action_ == GameAction::mine ? mine_request(std::string("mine") + verb, id_, config_.key, arguments_)
                : return_request(std::string("return") + verb, id_, config_.key, arguments_);
            pipe_ = std::make_unique<PipeExchange>(config_.channel, frame);
        }
        if (phase_ == Phase::begin) attempted_ = true;
        auto wire = pipe_->poll();
        if (!wire) return {};
        pipe_.reset();
        transport_failures_ = 0;
        if (action_ == GameAction::partner) {
            auto value = partner_reply(*wire, id_, config_.controller, skill_, arguments_);
            progress_ = std::move(value.progress);
            if (value.exited && (skill_ == "place" || skill_ == "fill_target")) return GameResult{true, value.placement_verified || value.changed ? GameEffect::observed :
                value.placement_attempted ? GameEffect::unknown : GameEffect::none,std::move(value.payload)};
            if (value.exited && skill_ == "craft") return GameResult{true, value.changed ? GameEffect::observed :
                value.craft_attempted ? GameEffect::unknown : GameEffect::none,std::move(value.payload)};
            if (value.exited && skill_ == "reply") return GameResult{true, value.delivery == ReplyDelivery::sent ? GameEffect::observed :
                value.delivery == ReplyDelivery::not_sent ? GameEffect::none : GameEffect::unknown,std::move(value.payload)};
            if (value.exited) return GameResult{true, skill_ == "observe" ? GameEffect::none :
                value.arrived || value.changed ? GameEffect::observed : GameEffect::unknown, std::move(value.payload)};
            if (defer && !stopping_) {
                phase_ = Phase::read;
                return GameResult{true, GameEffect::unknown, {}, true};
            }
        } else if (action_ == GameAction::inspect) {
            auto value = inspect_reply(*wire, id_, arguments_);
            if (value.exited) return GameResult{value.arrived, value.moved ? GameEffect::observed : GameEffect::none, std::move(value.payload)};
        } else if (action_ == GameAction::mine) {
            auto value = mine_reply(*wire, id_, arguments_);
            if (value.exited) return GameResult{value.finished, value.started ? GameEffect::unknown : GameEffect::none, std::move(value.payload)};
        } else {
            auto value = return_reply(*wire, id_, arguments_);
            if (value.exited) return GameResult{value.arrived, GameEffect::unknown, std::move(value.payload)};
        }
        phase_ = Phase::read;
        next_ = now + std::chrono::milliseconds(80);
    } catch (const PipeTransportError&) {
        pipe_.reset();
        next_ = now + std::chrono::milliseconds(150);
        if (stopping_) return {};
        // 提交已可能送达时，直接查询同一编号，避免重新发出游戏变更。
        if (attempted_) phase_ = Phase::read;
        // 收件正在核验同一绑定时，继续查原工作，避免短时共用传输故障提前触发停止。
        if (++transport_failures_ >= 3 && allow_begin) {
            if (!attempted_) throw;
            stopping_ = true;
        }
    } catch (...) {
        pipe_.reset();
        if (!attempted_ || phase_ == Phase::stop) throw;
        stopping_ = true;
    }
    return {};
}
}
