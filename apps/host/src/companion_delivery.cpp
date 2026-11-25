#include "companion_delivery.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
// 只由宿主保存的授权账目生成完整送达范围。
std::string grant_args(const CollectionGrant& grant) {
    return nlohmann::json{{"generation", grant.address.world}, {"question", std::to_string(grant.question)}, {"grant", std::to_string(grant.id)},
        {"owner", grant.owner}, {"order", grant.order}, {"quantity", grant.quantity}, {"material", grant.material},
        {"returnPlace", grant.return_place}, {"via", grant.entry == AnswerEntry::game ? "game" : "management"},
        {"command", std::to_string(grant.command)}}.dump();
}
}

// 错过接单回复后始终先查询；只有同范围的未接收事实可以重新投递。
void CompanionDelivery::finish(const CollectionGrant& grant) {
    status_.phase = DeliveryPhase::checking;
    const auto& outcome = work_->outcome();
    if (!outcome.succeeded || work_->fault() != agent::ToolError::none || outcome.output.size() > 16384) return;
    const auto value = nlohmann::json::parse(outcome.output, nullptr, false);
    if (!value.is_object() || !value.contains("grant") || value["grant"] != std::to_string(grant.id) ||
        !value.contains("question") || value["question"] != std::to_string(grant.question) ||
        !value.contains("generation") || value["generation"] != grant.address.world || !value.contains("phase")) return;
    if (checking_ && value["phase"] == "unreceived" && value.size() == 4) {
        status_.phase = status_.submissions < 3 ? DeliveryPhase::pending : DeliveryPhase::unavailable;
        return;
    }
    auto expected = nlohmann::json::parse(args_);
    expected["phase"] = "accepted";
    if (value == expected) {
        status_.receipt = outcome.output;
        status_.phase = DeliveryPhase::accepted;
    }
}

// 在宿主节拍中执行至多一份请求，原游戏地址失效后不转投新绑定。
// 步骤：结算已有工作的实际退出 → 核对原授权和世界 → 按接纳或回查状态提交。
void CompanionDelivery::advance(const std::optional<CollectionGrant>& grant, const std::optional<CompanionAddress>& address,
    const agent::ToolCatalog& catalog, const std::string& admit, const std::string& receipt, const agent::ToolClock& clock) {
    if (executor_) {
        executor_->advance();
        if (work_->exit()->state() == runtime::OperationState::pending) return;
        if (!closed_ && grant && grant->id == status_.grant) finish(*grant);
        work_.reset();
        executor_.reset();
        next_ = clock() + std::chrono::milliseconds(200);
    }
    if (closed_ || !grant || admit.empty() || receipt.empty()) return;
    if (grant->id != status_.grant) {
        args_ = grant_args(*grant);
        status_ = {grant->id, DeliveryPhase::pending, 0, 0, {}};
        next_ = {};
    }
    if (status_.phase == DeliveryPhase::accepted || status_.phase == DeliveryPhase::stale || status_.phase == DeliveryPhase::unavailable) return;
    if (!address) return;
    if (*address != grant->address) { status_.phase = DeliveryPhase::stale; return; }
    if (clock() < next_) return;
    checking_ = status_.phase == DeliveryPhase::checking;
    if (checking_ && status_.checks >= 8) { status_.phase = DeliveryPhase::unavailable; return; }
    const auto& tool = checking_ ? receipt : admit;
    auto scope = catalog.snapshot(std::span(&tool, 1), !checking_).bind_arguments(tool, args_);
    const auto definition = scope.definitions().front();
    const agent::RunId run{runtime::next_domain(), 1};
    auto executor = std::make_unique<agent::ToolExecutor>(run, std::move(scope), agent::ToolLimits{1, std::chrono::seconds(5)}, nullptr, clock);
    auto work = executor->submit({{run, 1}, 1}, {"dispatch", definition.name, args_});
    if (checking_) ++status_.checks; else ++status_.submissions;
    executor_ = std::move(executor);
    work_ = std::move(work);
}

// 停止新投递并关闭本地工作，未知接单结果保持不可核实。
void CompanionDelivery::close() {
    closed_ = true;
    if (executor_) executor_->close();
    if (status_.grant && status_.phase != DeliveryPhase::accepted) status_.phase = DeliveryPhase::unavailable;
}
}
