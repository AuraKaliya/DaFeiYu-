#include "host_state.hpp"
#include <dafeiyu/utf8.hpp>

namespace dfy::plugins {
// 固定宿主签发的所有者与服务寿命，C 调用无法另选身份。
void HostServices::enable_agents(std::weak_ptr<PluginAgents> service, const std::shared_ptr<PluginInstance>& instance,
    runtime::DomainId owner) {
    state_->require_owner();
    if (service.expired() || !instance || !owner || state_->agent_owner) throw std::invalid_argument("host: invalid agent grant");
    state_->agents = std::move(service);
    state_->agent_instance = instance;
    state_->agent_owner = owner;
}

// 检查尚未派发或仍有退出责任的回合。
bool HostServices::agents_busy() const {
    state_->require_owner();
    const auto service = state_->agents.lock();
    return service && service->busy(state_->agent_owner);
}
}

namespace dfy::plugins::detail {
// 先关闭原实例子会话接纳，再撤销能力持有。
void revoke_agents(HostState& host) noexcept {
    if (const auto service = host.agents.lock()) service->close_owner(host.agent_owner);
    host.agents.reset();
    host.agent_instance.reset();
}

namespace {
// 按字段上限复制借用值，空值不解引用。
std::string text(dfy_v2_string_view input, std::size_t capacity) {
    if (input.length > capacity || (!input.data && input.length)) throw std::invalid_argument("host: invalid agent text");
    const auto value = input.length ? std::string_view(input.data,input.length) : std::string_view{};
    if (!runtime::valid_utf8(value)) throw std::invalid_argument("host: agent text must be UTF-8");
    return std::string(value);
}

// 在有效原实例中申请独立会话。
dfy_v2_status DFY_V2_CALL open(uint64_t* output, dfy_v2_error* error) {
    if (output) *output = 0;
    return boundary(error,[&](auto& host) {
        if (!output) return DFY_V2_INVALID_ARGUMENT;
        const auto service = host.agents.lock();
        const auto instance = host.agent_instance.lock();
        if (!service || !instance) return DFY_V2_UNSUPPORTED;
        *output = service->open(instance);
        return DFY_V2_OK;
    });
}

// 校验并复制本轮输入，在宿主回调边界后才会开始执行。
// 步骤：清空输出及校验布局 → 核对服务 → 复制有界输入 → 接纳回合。
template<uint32_t version>
dfy_v2_status DFY_V2_CALL submit(uint64_t session, const dfy_v2_agent_input* input, uint64_t* request, dfy_v2_error* error) {
    if (request) *request = 0;
    return boundary(error,[&](auto& host) {
        constexpr auto thinking = DFY_V2_AGENT_THINKING_OFF | DFY_V2_AGENT_THINKING_LOW;
        constexpr auto flags = DFY_V2_AGENT_ALLOW_CHANGES | DFY_V2_AGENT_REPLACE_HISTORY |
            (version >= DFY_V2_AGENT_PERSONA_VERSION ? DFY_V2_AGENT_PLAYER_FACING : 0) |
            (version >= DFY_V2_AGENT_THINKING_VERSION ? thinking : 0);
        if (!session || !request || !input || input->tool_count > 16 || (!input->tools && input->tool_count) ||
            (input->flags & ~flags) || (input->flags & thinking) == thinking) return DFY_V2_INVALID_ARGUMENT;
        const auto service = host.agents.lock();
        if (!service) return DFY_V2_UNSUPPORTED;
        SubagentInput copied{text(input->prompt,64*1024),text(input->instructions,8*1024),text(input->binding,8*1024),{},
            (input->flags & DFY_V2_AGENT_ALLOW_CHANGES) != 0, (input->flags & DFY_V2_AGENT_REPLACE_HISTORY) != 0,
            (input->flags & DFY_V2_AGENT_PLAYER_FACING) != 0};
        if (input->flags & DFY_V2_AGENT_THINKING_OFF) copied.thinking = SubagentThinking::off;
        if (input->flags & DFY_V2_AGENT_THINKING_LOW) copied.thinking = SubagentThinking::low;
        copied.tools.reserve(input->tool_count);
        for (uint32_t index = 0; index < input->tool_count; ++index) copied.tools.push_back(text(input->tools[index],128));
        try { *request = service->submit(host.agent_owner,session,std::move(copied)); }
        catch (const std::invalid_argument&) { return DFY_V2_INVALID_ARGUMENT; }
        catch (const std::logic_error&) { return DFY_V2_BUSY; }
        return DFY_V2_OK;
    });
}

// 取得原请求当前阶段与只读正文，不推进后台工作。
// 步骤：检查输出与身份 → 查询状态 → 复制阶段及借用视图。
dfy_v2_status DFY_V2_CALL status(uint64_t session, dfy_v2_agent_status* output, dfy_v2_error* error) {
    if (output) *output = {};
    return boundary(error,[&](auto& host) {
        if (!session || !output) return DFY_V2_INVALID_ARGUMENT;
        const auto service = host.agents.lock();
        if (!service) return DFY_V2_UNSUPPORTED;
        const auto value = service->status(host.agent_owner,session);
        constexpr uint32_t phases[] = {DFY_V2_AGENT_IDLE,DFY_V2_AGENT_QUEUED,DFY_V2_AGENT_RUNNING,
            DFY_V2_AGENT_SUCCEEDED,DFY_V2_AGENT_FAILED,DFY_V2_AGENT_CANCELLED};
        *output = {value.request,phases[static_cast<std::size_t>(value.phase)],0,{value.answer.data(),value.answer.size()}};
        return DFY_V2_OK;
    });
}

// 仅向当前实例的指定请求发送取消信号。
dfy_v2_status DFY_V2_CALL cancel(uint64_t session, uint64_t request, dfy_v2_error* error) {
    return boundary(error,[&](auto& host) {
        if (!session || !request) return DFY_V2_INVALID_ARGUMENT;
        const auto service = host.agents.lock();
        if (!service) return DFY_V2_UNSUPPORTED;
        return service->cancel(host.agent_owner,session,request) ? DFY_V2_OK : DFY_V2_NOT_FOUND;
    });
}

// 请求关闭原会话，由宿主继续推进实际退出。
dfy_v2_status DFY_V2_CALL close(uint64_t session, dfy_v2_error* error) {
    return boundary(error,[&](auto& host) {
        if (!session) return DFY_V2_INVALID_ARGUMENT;
        const auto service = host.agents.lock();
        if (!service) return DFY_V2_UNSUPPORTED;
        service->close(host.agent_owner,session);
        return DFY_V2_OK;
    });
}
}

// 发布借用服务的精确 ABI 表。
const dfy_v2_agent_api& agent_api(uint32_t version) {
    static const dfy_v2_agent_api basic{{sizeof(dfy_v2_agent_api),DFY_V2_AGENT_VERSION},open,submit<DFY_V2_AGENT_VERSION>,status,cancel,close};
    static const dfy_v2_agent_api persona{{sizeof(dfy_v2_agent_api),DFY_V2_AGENT_PERSONA_VERSION},open,
        submit<DFY_V2_AGENT_PERSONA_VERSION>,status,cancel,close};
    static const dfy_v2_agent_api thinking{{sizeof(dfy_v2_agent_api),DFY_V2_AGENT_THINKING_VERSION},open,
        submit<DFY_V2_AGENT_THINKING_VERSION>,status,cancel,close};
    if (version == DFY_V2_AGENT_THINKING_VERSION) return thinking;
    return version == DFY_V2_AGENT_PERSONA_VERSION ? persona : basic;
}
}
