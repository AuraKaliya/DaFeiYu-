#include "companion_mining.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace dfy::app {
// 按有界块编号结算一次；来源缺失保留范围，已经使用的产出不能重复增加数量。
// 步骤：读取已验证的上下界 → 核对来源未用过 → 累加到本次委托。
void CompanionMining::credit(const std::string& evidence) {
    const auto value = nlohmann::json::parse(evidence, nullptr, false);
    if (!value.is_object() || !value.contains("pickup") || !value.at("pickup").is_object()) {
        ++status_.upper;
        return;
    }
    const auto& pickup = value.at("pickup");
    const auto lower = pickup.at("lower").get<std::uint64_t>(), upper = pickup.at("upper").get<std::uint64_t>();
    if (lower > upper || upper > 1) { ++status_.upper; return; }
    const auto source = pickup.at("stream").get<std::string>() + "/" + std::to_string(pickup.at("first").get<std::uint64_t>());
    if (std::find(status_.pickup_sources.begin(), status_.pickup_sources.end(), source) != status_.pickup_sources.end()) return;
    status_.pickup_sources.push_back(source);
    status_.lower += lower;
    status_.upper += upper;
}

// 将到场、实际不足和证据存疑分别保存，不用采够时的旧快照完成交付。
void CompanionMining::settle_return(const CollectionGrant& grant) {
    auto& home = status_.home;
    home.phase = ReturnPhase::failed;
    const auto& result = work_->outcome();
    if (!result.succeeded || work_->fault() != agent::ToolError::none || result.output.size() > 16384) return;
    try {
        const auto value = nlohmann::json::parse(result.output);
        if (value.at("generation") != grant.address.world || value.at("grant") != std::to_string(grant.id) ||
            value.at("question") != std::to_string(grant.question) || value.at("place") != grant.return_place ||
            value.at("phase") != "arrived" || value.at("exited") != true || !value.at("stock").is_number_unsigned()) return;
        home.stock = value.at("stock").get<std::uint64_t>();
        home.evidence = result.output;
        home.phase = ReturnPhase::uncertain;
        const auto& proof = value.at("carried");
        if (proof.is_null() || !proof.at("lower").is_number_unsigned() || !proof.at("upper").is_number_unsigned()) return;
        const auto lower = proof.at("lower").get<std::uint64_t>(), upper = proof.at("upper").get<std::uint64_t>();
        const auto prefix = proof.at("stream").get<std::string>() + "/";
        if (lower > upper || lower > status_.lower || upper > status_.upper || upper > home.stock ||
            proof.at("stock") != home.stock || status_.pickup_sources.empty() || !std::all_of(status_.pickup_sources.begin(),
            status_.pickup_sources.end(), [&](const auto& source) { return source.starts_with(prefix); })) return;
        home.lower = lower;
        home.upper = upper;
        if (lower >= grant.quantity) home.phase = ReturnPhase::delivered;
        else if (upper < grant.quantity) home.phase = ReturnPhase::shortfall;
    } catch (const std::exception&) { home.phase = ReturnPhase::uncertain; }
}
// 从已接单范围提交采掘并读取原世界、原委托、原块的真实游戏回执。
// 步骤：复核接单与绑定 → 推进在途工作或协作取消 → 验证客户端退出证据 → 按配额接纳首块。
void CompanionMining::advance(const std::optional<CollectionGrant>& grant, const DeliveryStatus& delivery,
    const std::optional<CompanionAddress>& address, const agent::ToolCatalog& catalog, const std::string& tool,
    const std::string& return_tool, const agent::ToolClock& clock) {
    const bool valid = !closed_ && grant && address && *address == grant->address &&
        grant->id == delivery.grant && delivery.phase == DeliveryPhase::accepted;
    if (executor_) {
        if (!valid) executor_->close();
        executor_->advance();
        if (work_->exit()->state() == runtime::OperationState::pending) return;
        if (returning_) {
            if (valid && grant->id == status_.grant) settle_return(*grant);
            else status_.home.phase = ReturnPhase::uncertain;
            work_.reset();
            executor_.reset();
            returning_ = false;
            return;
        }
        const auto& result = work_->outcome();
        status_.effect = result.effect;
        status_.phase = valid ? MiningPhase::failed : MiningPhase::withdrawn;
        if (grant && grant->id == status_.grant && result.output.size() <= 16384) {
            try {
                const auto value = nlohmann::json::parse(result.output);
                if (value.at("generation") == grant->address.world && value.at("question") == std::to_string(grant->question) &&
                    value.at("grant") == std::to_string(grant->id) && value.at("index") == status_.index && value.at("exited") == true) {
                    status_.started = value.at("started").get<bool>();
                    status_.evidence = result.output;
                    if (valid && result.succeeded && status_.started && value.at("phase") == "clientFinished" && work_->fault() == agent::ToolError::none) {
                        credit(result.output);
                        status_.phase = MiningPhase::client_finished;
                    }
                }
            } catch (const std::exception&) { }
        }
        work_.reset();
        executor_.reset();
        return;
    }
    if (!valid || tool.empty() || grant->quantity == 0) return;
    const bool same = status_.grant == grant->id;
    if (same && status_.phase != MiningPhase::client_finished) return;
    if (same && status_.index >= grant->quantity) {
        if (return_tool.empty() || status_.home.phase != ReturnPhase::idle) return;
        const auto args = nlohmann::json{{"generation", grant->address.world}, {"question", std::to_string(grant->question)},
            {"grant", std::to_string(grant->id)}}.dump();
        auto scope = catalog.snapshot(std::span(&return_tool, 1), true).bind_arguments(return_tool, args);
        const auto name = scope.definitions().front().name;
        const agent::RunId run{runtime::next_domain(), 1};
        auto executor = std::make_unique<agent::ToolExecutor>(run, std::move(scope), agent::ToolLimits{1, std::chrono::seconds(35)}, nullptr, clock);
        auto work = executor->submit({{run, 1}, 1}, {"return", name, args});
        status_.home.phase = ReturnPhase::requested;
        executor_ = std::move(executor);
        work_ = std::move(work);
        returning_ = true;
        return;
    }
    const auto index = same ? status_.index + 1 : 1;
    const auto args = nlohmann::json{{"generation", grant->address.world}, {"question", std::to_string(grant->question)},
        {"grant", std::to_string(grant->id)}, {"index", index}}.dump();
    auto scope = catalog.snapshot(std::span(&tool, 1), true).bind_arguments(tool, args);
    const auto definition = scope.definitions().front();
    const agent::RunId run{runtime::next_domain(), 1};
    auto executor = std::make_unique<agent::ToolExecutor>(run, std::move(scope), agent::ToolLimits{1, std::chrono::seconds(35)}, nullptr, clock);
    auto work = executor->submit({{run, 1}, 1}, {"mine", definition.name, args});
    if (!same) status_ = {};
    status_.grant = grant->id;
    status_.index = index;
    status_.phase = MiningPhase::submitted;
    status_.started = false;
    status_.effect = agent::ToolEffect::not_applied;
    status_.evidence.clear();
    executor_ = std::move(executor);
    work_ = std::move(work);
}

// 关闭只发起一次协作收尾，不据此生成掉落或入包结论。
void CompanionMining::close() {
    closed_ = true;
    if (executor_) executor_->close();
}
}
