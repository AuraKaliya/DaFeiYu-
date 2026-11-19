#include "companion_inbox.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 显式开启玩家收件，同一角色重复开启保留原游标。
void CompanionInbox::enable(std::string tool) {
    if (status_.enabled && tool != tool_) throw std::logic_error("companion: inbox already bound");
    tool_ = std::move(tool);
    status_.enabled = true;
}
// 换世界时旧原话和读取结果不能在新地址生效。
void CompanionInbox::bind(std::optional<CompanionAddress> address) {
    if (address == address_) return;
    if (executor_) executor_->close();
    address_ = std::move(address);
    entries_.clear();
    status_ = {status_.enabled, false, false, 0, {}};
    next_ = {};
}

// 首次绑定只取水位，后续连续记录才允许进入宿主委托判断。
// 步骤：检查原玩家及世界 → 识别初次读取、断档或溢出 → 暂存整批并提交游标。
void CompanionInbox::accept(const std::string& payload) {
    if (payload.size() > 16384) throw std::invalid_argument("companion: oversized inbox");
    const auto batch = nlohmann::json::parse(payload);
    if (batch.is_null() || batch.at("generation") != address_->world) throw std::invalid_argument("companion: inactive inbox");
    const auto owner = batch.at("owner").get<std::string>();
    const auto latest = batch.at("latest").get<std::uint64_t>();
    const auto& messages = batch.at("messages");
    if (owner.empty() || owner.size() > 128 || !messages.is_array() || messages.size() > 8 || latest < status_.cursor ||
        (status_.ready && owner != status_.owner)) throw std::invalid_argument("companion: invalid inbox");
    if (batch.contains("control")) {
        const auto& record = batch.at("control");
        const auto order = record.at("order").get<std::uint64_t>();
        if (record.at("generation") != address_->world || !record.at("order").is_number_unsigned() || order > latest)
            throw std::invalid_argument("companion: invalid control receipt");
        const auto& phase = record.at("phase");
        if (phase != "released" && phase != "requested" && phase != "unconfirmed") throw std::invalid_argument("companion: invalid control phase");
        status_.control = ControlReceipt{address_->world, order, phase == "released" ? ControlPhase::released :
            phase == "requested" ? ControlPhase::requested : ControlPhase::unconfirmed};
    }
    if (!status_.ready || batch.at("gap").get<bool>() || entries_.size() + messages.size() > 32) {
        status_.gap = status_.gap || status_.ready;
        status_.ready = true;
        status_.owner = owner;
        status_.cursor = latest;
        entries_.clear();
        return;
    }
    auto cursor = status_.cursor;
    auto pending = entries_;
    for (const auto& message : messages) {
        PlayerUtterance entry{message.at("sequence").get<std::uint64_t>(), message.at("text").get<std::string>(), {}, {}, {}};
        if (entry.sequence != ++cursor || cursor > latest || entry.text.empty() || entry.text.size() > 1024)
            throw std::invalid_argument("companion: invalid player message");
        if (message.contains("offer") && message.at("offer").at("purpose") == "visit.inspect")
            entry.visit_place = message.at("offer").at("place").get<std::string>();
        if (message.contains("offer") && message.at("offer").at("purpose") == "question.decline")
            entry.declined_question = message.at("offer").at("question").get<std::string>();
        if (message.contains("offer") && message.at("offer").at("purpose") == "collect.return") {
            const auto& offer = message.at("offer");
            if (!offer.at("quantity").is_number_unsigned() || offer.at("quantity") == 0 || offer.at("quantity") > 64)
                throw std::invalid_argument("companion: invalid proposed quantity");
            entry.quantity = offer.at("quantity").get<std::uint64_t>();
            entry.collection_question = offer.at("question").get<std::string>();
        }
        pending.push_back(std::move(entry));
    }
    if ((cursor < latest) != batch.at("more").get<bool>()) throw std::invalid_argument("companion: invalid inbox continuation");
    entries_.swap(pending);
    status_.cursor = cursor;
}

// 通过真正登记的只读工具读取玩家增量，守住原世界及原实例。
// 步骤：推进在途读取 → 处理成功或失联 → 仅在当前地址安排下次查询。
void CompanionInbox::advance(const agent::ToolCatalog& catalog, const agent::ToolClock& clock) {
    if (executor_) {
        executor_->advance();
        if (work_->exit()->state() == runtime::OperationState::pending) return;
        if (address_ && reading_ == address_) {
            try {
                if (!work_->outcome().succeeded || work_->fault() != agent::ToolError::none)
                    throw std::runtime_error("companion: private read failed");
                accept(work_->outcome().output);
            } catch (const std::exception&) {
                status_.ready = false;
                status_.gap = true;
                entries_.clear();
            }
        }
        work_.reset();
        executor_.reset();
        reading_.reset();
        next_ = clock() + std::chrono::milliseconds(200);
    }
    if (!status_.enabled || !address_ || clock() < next_) return;
    const auto scope = catalog.snapshot(std::span(&tool_, 1), false);
    const auto definitions = scope.definitions();
    if (definitions.size() != 1) throw std::logic_error("companion: invalid inbox scope");
    const agent::RunId run{runtime::next_domain(), 1};
    auto executor = std::make_unique<agent::ToolExecutor>(run, scope, agent::ToolLimits{1, std::chrono::seconds(4)}, nullptr, clock);
    auto args = nlohmann::json{{"generation", address_->world}, {"after", status_.cursor}}.dump();
    auto work = executor->submit({{run, 1}, 1}, {"player-inbox", definitions.front().name, std::move(args)});
    reading_ = address_;
    executor_ = std::move(executor);
    work_ = std::move(work);
}

// 从本代玩家窗口取出下一条已确认原话。
std::optional<PlayerUtterance> CompanionInbox::take() {
    if (entries_.empty()) return {};
    auto next = std::move(entries_.front());
    entries_.pop_front();
    return next;
}
}
