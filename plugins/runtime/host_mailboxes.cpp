#include "host_state.hpp"

namespace dfy::plugins {
// 固定实例与本体身份，待插件显式接入时才占用信箱槽位。
void HostServices::enable_mailboxes(std::shared_ptr<events::Mailboxes> network, runtime::DomainId app, runtime::DomainId plugin) {
    state_->require_owner();
    if (!network || !app || !plugin || app == plugin || state_->mailboxes ||
        state_->resources.state() != runtime::OwnerState::open) throw std::invalid_argument("host: invalid mailbox grant");
    state_->mailboxes = std::move(network);
    state_->mailbox_app = app;
    state_->mailbox_plugin = plugin;
}

// 读取已接入实例的地址副本，启用服务或加载包本身不创建地址。
dfy_v2_mailbox_pair HostServices::mailbox_addresses() const {
    state_->require_owner();
    return state_->mailbox_pair;
}
}

namespace dfy::plugins::detail {
// 停止该插件参与的双向投递，已读载荷继续遵循原引用寿命。
void revoke_mailboxes(HostState& host) noexcept {
    if (host.mailboxes) host.mailboxes->revoke(host.mailbox_plugin);
    host.mailbox_pair = {};
    host.mailboxes.reset();
}

namespace {
// 以当前实例身份建立并复用一对信箱地址。
// 步骤：校验输出与宿主接纳 → 建立双向绑定或回滚 → 返回原地址。
dfy_v2_status DFY_V2_CALL attach(dfy_v2_mailbox_pair* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        if (!host.mailboxes) return DFY_V2_UNSUPPORTED;
        if (host.resources.state() != runtime::OwnerState::open) return DFY_V2_BUSY;
        if (!host.mailbox_pair.inbound) {
            const auto inbound = host.mailboxes->open(host.mailbox_plugin,host.mailbox_app);
            try {
                const auto outbound = host.mailboxes->open(host.mailbox_app,host.mailbox_plugin);
                host.mailbox_pair = {inbound.value,outbound.value};
            } catch (...) {
                host.mailboxes->close(host.mailbox_plugin,inbound);
                throw;
            }
        }
        *output = host.mailbox_pair;
        return DFY_V2_OK;
    });
}

// 将分类及文本复制为宿主不可变载荷。
std::shared_ptr<const runtime::StructValue> make_text(dfy_v2_string_view category, uint32_t version,
    dfy_v2_string_view text, std::size_t limit) {
    using namespace runtime;
    constexpr std::size_t name_limit = 128;
    if (!category.data || !category.length || category.length > name_limit || !version || (!text.data && text.length))
        throw std::invalid_argument("host: invalid mailbox message");
    if (text.length > limit) throw std::length_error("host: mailbox text exceeds capacity");
    const FieldDescriptor fields[] = {{"text",ValueKind::string,limit}};
    auto descriptor = std::make_shared<const StructDescriptor>(
        StructDescriptor::create({category.data,category.length},version,fields,{1,name_limit+4}));
    const NamedValue contents[] = {{"text",Value::from_string(text.data ? std::string_view{text.data,text.length} : std::string_view{},limit)}};
    return std::make_shared<const StructValue>(StructValue::create(std::move(descriptor),contents,limit));
}

// 写入当前实例的出站信箱，C 消费者不提交或伪造发送者身份。
// 步骤：校验并清空输出 → 准备宿主载荷 → 投递或复用回执 → 转换接纳状态。
dfy_v2_status DFY_V2_CALL post(uint64_t sequence, dfy_v2_string_view category, uint32_t version,
    dfy_v2_string_view text, dfy_v2_mailbox_receipt* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output || !sequence) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        if (!host.mailbox_pair.outbound) return DFY_V2_UNSUPPORTED;
        const auto receipt = host.mailboxes->deliver(host.mailbox_plugin,{host.mailbox_pair.outbound},sequence,
            make_text(category,version,text,host.mailboxes->payload_limit()));
        uint32_t state = 0;
        switch (receipt.state) {
        case events::DeliveryState::accepted: state = DFY_V2_MAILBOX_ACCEPTED; break;
        case events::DeliveryState::repeated: state = DFY_V2_MAILBOX_REPEATED; break;
        case events::DeliveryState::expired: state = DFY_V2_MAILBOX_EXPIRED; break;
        case events::DeliveryState::out_of_order: state = DFY_V2_MAILBOX_OUT_OF_ORDER; break;
        }
        *output = {state,0,receipt.event.domain,receipt.event.sequence};
        return DFY_V2_OK;
    });
}

// 只读取当前实例入站首件，引用分配失败时可重试同一消息。
// 步骤：查询队首 → 预先签发引用并准备寿命持有 → 移出原件 → 交付既定回执。
dfy_v2_status DFY_V2_CALL poll(dfy_v2_mailbox_message* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        if (!host.mailbox_pair.inbound) return DFY_V2_UNSUPPORTED;
        const events::MailboxId box{host.mailbox_pair.inbound};
        const auto* front = host.mailboxes->front(host.mailbox_plugin,box);
        if (!front) return DFY_V2_NOT_FOUND;

        const auto id = front->id;
        auto held = std::make_shared<events::EventLease>();
        const auto payload = store_object(host,std::shared_ptr<const runtime::StructValue>(held,front->payload.get()));
        *held = std::move(host.mailboxes->read(host.mailbox_plugin,box).event);
        *output = {id.domain,id.sequence,payload};
        return DFY_V2_OK;
    });
}
}

// 返回信箱接入及读写能力的固定版本表。
const dfy_v2_mailbox_api& mailbox_api() {
    static const dfy_v2_mailbox_api api{{sizeof(dfy_v2_mailbox_api),DFY_V2_MAILBOX_VERSION},attach,post,poll};
    return api;
}
}
