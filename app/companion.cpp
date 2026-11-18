#include "companion.hpp"

namespace dfy::app {
namespace {
// 要求实例自身提供经工具目录核验的角色声明。
plugins::CompanionOffer require_offer(plugins::PluginInstance& instance, const agent::ToolCatalog& catalog) {
    auto offer = plugins::companion_offer(instance, catalog);
    if (!offer) throw std::invalid_argument("companion: plugin offers no companion role");
    return std::move(*offer);
}
}

// 为原插件建立宿主拥有的独立上下文，构造失败自动撤销临时注册。
// 步骤：登记当前实例工具 → 验证可选角色声明 → 创建独立会话。
CompanionSession::CompanionSession(const std::shared_ptr<plugins::PluginInstance>& instance, const model::Config& config,
    const std::shared_ptr<events::BroadcastChannel>& channel, const model::Backend& backend, const agent::RunVersionLabels& labels)
    : instance_(instance), package_(instance->plugin_id()), catalog_(std::make_shared<agent::ToolCatalog>()),
      registration_(instance, catalog_), offer_(require_offer(*instance, *catalog_)), session_(config, channel, backend),
      conversation_(session_, labels) {}

// 重新核对原代次与活动状态，弱引用不阻止空闲 DLL 卸载。
bool CompanionSession::bound() const noexcept {
    const auto instance = instance_.lock();
    return instance && instance->generation() == offer_.generation && instance->phase() == plugins::InstancePhase::active;
}

// 按插件声明的只读入口向游戏中的同一角色委托一个回合。
// 步骤：核对会话与来源 → 固定本轮工具和时钟 → 接纳后清理上一回执。
agent::TurnDiagnostics CompanionSession::submit(const std::string& prompt, const agent::ToolClock& clock) {
    if (stopping_ || !bound()) throw std::logic_error("companion: binding unavailable");
    agent::RunOptions options;
    options.now = clock;
    options.tools = catalog_->snapshot(std::span(&offer_.state_tool, 1), false);
    const auto admitted = conversation_.submit(prompt, std::move(options));
    result_.reset();
    return admitted;
}

// 只推进本会话并保留首次业务结算，来源失效后进入原运行收尾。
void CompanionSession::advance() {
    if (!stopping_ && !bound()) close();
    reader_.advance();
    if (auto completed = conversation_.advance()) result_ = std::move(completed);
}

// 先撤销新工具接纳，再关闭会话并保留实际退出责任。
void CompanionSession::close() {
    if (stopping_) return;
    stopping_ = true;
    registration_.revoke();
    reader_.revoke();
    conversation_.close();
}

// 从实际注册中重新取得只读范围，不能向被替换的实例读取。
std::uint64_t CompanionSession::refresh(const agent::ToolClock& clock) {
    if (stopping_ || !bound()) throw std::logic_error("companion: binding unavailable");
    return reader_.refresh(catalog_->snapshot(std::span(&offer_.state_tool, 1), false), clock);
}

// 获取可跨界面复制的最新回合状态。
CompanionStatus CompanionSession::status() const {
    return {id(), offer_.generation, package_, offer_.state_topic, stopping_, closed(), conversation_.status()};
}

// 仅返回仍与原回合匹配的可信结算结果。
std::optional<CompletedTurn> CompanionSession::result(std::uint64_t turn) const {
    if (result_ && result_->status.turn == turn) return result_;
    return {};
}
}
