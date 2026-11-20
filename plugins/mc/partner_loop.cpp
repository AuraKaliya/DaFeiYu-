#include "partner_loop.hpp"
#include <array>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;

// 借出仅本次 ABI 调用使用的字符串。
constexpr dfy_v2_string_view view(std::string_view bytes) noexcept { return {bytes.data(),bytes.size()}; }
constexpr std::array skills{view("mc.companion/observe"),view("mc.companion/move"),
    view("mc.companion/collect"),view("mc.companion/reply")};
constexpr auto instructions = view("你是大肥鱼，用自己的角色和玩家一起玩。events 是按序收到的在场变化和玩家原话。"
    "结合当前约定、自己的打算和已确认的现场，自行选择行动、协商、拒绝或安静等待。"
    "要对玩家说话时使用回复工具；回合结束的文本是内部结算，不会发到游戏。");
}

// 由玩家入口显式启用，插件加载本身不读取游戏或提交模型。
uint32_t PartnerLoop::listen(dfy_v2_query_interface_fn host, const BridgeConfig& config) {
    if (phase_ == DFY_V2_PARTNER_LISTENING) return DFY_V2_OK;
    if (phase_ == DFY_V2_PARTNER_STOPPING) return DFY_V2_BUSY;
    if (config.key.empty()) return DFY_V2_UNSUPPORTED;
    const void* api = nullptr;
    const auto result = host(view(DFY_V2_AGENT_INTERFACE),DFY_V2_AGENT_VERSION,&api,nullptr);
    if (result || !api) return result ? result : DFY_V2_UNSUPPORTED;
    const auto& agent = *static_cast<const dfy_v2_agent_api*>(api);
    if (agent.header.struct_size != sizeof(agent) || agent.header.interface_version != DFY_V2_AGENT_VERSION ||
        !agent.open || !agent.submit || !agent.status || !agent.cancel || !agent.close) return DFY_V2_ABI_MISMATCH;
    agent_ = &agent;
    phase_ = DFY_V2_PARTNER_LISTENING;
    return DFY_V2_OK;
}

// 保存协作取消责任，下一步停止读取并等待在途工作退出。
void PartnerLoop::stop() noexcept { if (phase_ == DFY_V2_PARTNER_LISTENING) phase_ = DFY_V2_PARTNER_STOPPING; }

// 在宿主已撤销全部子会话工作后释放插件本地读口。
void PartnerLoop::reset() noexcept {
    feed_.clear();
    phase_ = DFY_V2_PARTNER_CLOSED;
    agent_ = nullptr;
    session_world_.clear();
    session_ = request_ = order_ = settled_ = 0;
}

// 子回合业务结算及真正退出均由原会话报告。
// 步骤：核对请求 → 按需要提出取消 → 保留在途状态或记录实际退出水位。
bool PartnerLoop::settle(bool cancel) {
    if (!request_) return true;
    dfy_v2_agent_status state{};
    const auto result = agent_->status(session_,&state,nullptr);
    if (result == DFY_V2_INVALID_ARGUMENT && cancel) { request_ = 0; return true; }
    if (result || state.request != request_) throw std::runtime_error("MC partner: original turn unavailable");
    if (state.phase == DFY_V2_AGENT_QUEUED || state.phase == DFY_V2_AGENT_RUNNING) {
        if (cancel) agent_->cancel(session_,request_,nullptr);
        return false;
    }
    settled_ = order_;
    request_ = 0;
    return true;
}

// 原会话空闲后释放旧游戏上下文，下一世界重新签发身份。
void PartnerLoop::retire() {
    if (session_) agent_->close(session_,nullptr);
    session_world_.clear();
    session_ = order_ = settled_ = 0;
}

// 将按序收到的原始事实及绑定游戏数据送入同一子会话。
// 步骤：创建或复用上下文 → 定格原玩家及候选范围 → 接纳后消费事实。
void PartnerLoop::submit() {
    if (!session_) {
        auto world = feed_.world();
        if (agent_->open(&session_,nullptr)) throw std::runtime_error("MC partner: cannot open child session");
        session_world_ = std::move(world);
    }
    const auto& facts = feed_.pending();
    const auto order = facts.back().at("sequence").get<std::uint64_t>();
    Json input{{"player",feed_.owner()},{"generation",session_world_},{"events",facts},{"lastObservation",feed_.observation()}};
    if (feed_.gap()) input["eventGap"] = true;
    const auto prompt = input.dump();
    const auto binding = Json{{"generation",session_world_},{"order",order}}.dump();
    const dfy_v2_agent_input run{view(prompt),instructions,view(binding),skills.data(),
        static_cast<uint32_t>(skills.size()),DFY_V2_AGENT_ALLOW_CHANGES};
    if (agent_->submit(session_,&run,&request_,nullptr)) throw std::runtime_error("MC partner: cannot queue child turn");
    order_ = order;
    feed_.consumed();
}

// 驱动插件自己的事件与行动循环，游戏解绑优先结束原请求。
// 步骤：推进原生读取 → 核对在途回合 → 释放旧上下文或接纳最新事件。
void PartnerLoop::advance(const BridgeConfig& config, std::uint64_t instance) {
    if (phase_ == DFY_V2_PARTNER_CLOSED) return;
    const bool closing = phase_ == DFY_V2_PARTNER_STOPPING;
    if (closing) feed_.clear();
    else feed_.advance(config,instance);
    const bool replaced = session_ && feed_.world() != session_world_;
    if (!settle(closing || replaced)) return;
    if (closing) { retire(); reset(); return; }
    if (replaced) retire();
    if (feed_.ready() && !feed_.pending().empty()) submit();
}

// 借出最新角色状态，世界字符串只在本次 ABI 调用内有效。
dfy_v2_partner_status PartnerLoop::status() const noexcept {
    return {phase_,feed_.ready(),session_,feed_.cursor(),feed_.departed(),settled_,view(feed_.world()),view(feed_.owner())};
}
}
