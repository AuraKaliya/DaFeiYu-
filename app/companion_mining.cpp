#include "companion_mining.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 从已接单范围提交采掘并读取原世界、原委托、原块的真实游戏回执。
// 步骤：复核接单与绑定 → 推进在途工作或协作取消 → 验证客户端退出证据 → 按配额接纳首块。
void CompanionMining::advance(const std::optional<CollectionGrant>& grant, const DeliveryStatus& delivery,
    const std::optional<CompanionAddress>& address, const agent::ToolCatalog& catalog, const std::string& tool,
    const agent::ToolClock& clock) {
    const bool valid = !closed_ && grant && address && *address == grant->address &&
        grant->id == delivery.grant && delivery.phase == DeliveryPhase::accepted;
    if (executor_) {
        if (!valid) executor_->close();
        executor_->advance();
        if (work_->exit()->state() == runtime::OperationState::pending) return;
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
                    if (valid && result.succeeded && status_.started && value.at("phase") == "clientFinished" && work_->fault() == agent::ToolError::none)
                        status_.phase = MiningPhase::client_finished;
                }
            } catch (const std::exception&) { }
        }
        work_.reset();
        executor_.reset();
        return;
    }
    if (!valid || tool.empty() || status_.grant == grant->id || grant->quantity == 0) return;
    const auto args = nlohmann::json{{"generation", grant->address.world}, {"question", std::to_string(grant->question)},
        {"grant", std::to_string(grant->id)}, {"index", 1}}.dump();
    auto scope = catalog.snapshot(std::span(&tool, 1), true).bind_arguments(tool, args);
    const auto definition = scope.definitions().front();
    const agent::RunId run{runtime::next_domain(), 1};
    auto executor = std::make_unique<agent::ToolExecutor>(run, std::move(scope), agent::ToolLimits{1, std::chrono::seconds(35)}, nullptr, clock);
    auto work = executor->submit({{run, 1}, 1}, {"mine", definition.name, args});
    status_ = {grant->id, 1, MiningPhase::submitted, false, agent::ToolEffect::not_applied, {}};
    executor_ = std::move(executor);
    work_ = std::move(work);
}

// 关闭只发起一次协作收尾，不据此生成掉落或入包结论。
void CompanionMining::close() {
    closed_ = true;
    if (executor_) executor_->close();
}
}
