#include "game_work.hpp"
#include <stdexcept>

namespace dfy::mc {
namespace {
// 按技能解释已退出动作的真实效果，正常轮询和迟到读回复用同一标准。
// 步骤：按技能核对已观察的变化或投递状态 → 交付原业务正文。
GameResult terminal_result(std::string_view skill, PartnerReply value) {
    auto effect = GameEffect::unknown;
    if (skill == "place" || skill == "fill_target")
        effect = value.placement_verified || value.changed ? GameEffect::observed : value.placement_attempted ? GameEffect::unknown : GameEffect::none;
    else if (skill == "craft" || skill == "pickup_drop" || skill == "give_item" || skill == "store_item" || skill == "withdraw_item")
        effect = value.changed ? GameEffect::observed : (value.craft_attempted || value.inventory_attempted) ? GameEffect::unknown : GameEffect::none;
    else if (skill == "reply")
        effect = value.delivery == ReplyDelivery::sent ? GameEffect::observed : value.delivery == ReplyDelivery::not_sent ? GameEffect::none : GameEffect::unknown;
    else if (skill == "observe") effect = GameEffect::none;
    else if (value.arrived || value.changed) effect = GameEffect::observed;
    return {true,effect,std::move(value.payload)};
}
}

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

// 脱离本地票据后，沿原编号、控制器、世界和参数核对游戏是否退出。
// 步骤：释放旧收发口 → 只读原工作 → 接受已校验的终态或延后重查。
std::optional<GameResult> GameWork::read_exit() {
    if (!attempted_ || action_ != GameAction::partner) return {};
    if (phase_ != Phase::verify_exit) {
        pipe_.reset();
        next_ = {};
        phase_ = Phase::verify_exit;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!pipe_ && now < next_) return {};
    try {
        if (!pipe_) pipe_ = std::make_unique<PipeExchange>(config_.channel,partner_request("partner.receipt",id_,config_,skill_,arguments_));
        auto wire = pipe_->poll();
        if (!wire) return {};
        pipe_.reset();
        auto value = partner_reply(*wire,id_,config_.controller,skill_,arguments_);
        if (value.exited) {
            exited_ = true;
            return terminal_result(skill_,std::move(value));
        }
    } catch (const std::exception&) { pipe_.reset(); }
    next_ = now + std::chrono::seconds(1);
    return {};
}

// 每轮最多处理一次非等待本机收发，桥接异常后尝试停止同一动作。
// 步骤：处理取消和期限 → 开工前登记宿主责任 → 提交或查询原命令 → 核对实际退出。
std::optional<GameResult> GameWork::poll(bool cancelled, bool defer, bool allow_begin,
    const std::function<GameAdmission()>& before_begin) {
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
            if (phase_ == Phase::begin && before_begin) {
                if (!admission_) admission_ = before_begin();
                if (admission_->blocked) return admission_->blocked;
            }
            pipe_ = std::make_unique<PipeExchange>(config_.channel, frame);
        }
        if (phase_ == Phase::begin) attempted_ = true;
        auto wire = pipe_->poll();
        if (!wire) return {};
        pipe_.reset();
        transport_failures_ = 0;
        if (action_ == GameAction::partner) {
            auto value = partner_reply(*wire, id_, config_.controller, skill_, arguments_);
            exited_ = value.exited;
            progress_ = std::move(value.progress);
            if (value.exited) return terminal_result(skill_,std::move(value));
            if (defer && !stopping_) {
                phase_ = Phase::read;
                return GameResult{true, GameEffect::unknown, {}, true};
            }
        } else if (action_ == GameAction::inspect) {
            auto value = inspect_reply(*wire, id_, arguments_);
            exited_ = value.exited;
            if (value.exited) return GameResult{value.arrived, value.moved ? GameEffect::observed : GameEffect::none, std::move(value.payload)};
        } else if (action_ == GameAction::mine) {
            auto value = mine_reply(*wire, id_, arguments_);
            exited_ = value.exited;
            if (value.exited) return GameResult{value.finished, value.started ? GameEffect::unknown : GameEffect::none, std::move(value.payload)};
        } else {
            auto value = return_reply(*wire, id_, arguments_);
            exited_ = value.exited;
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
