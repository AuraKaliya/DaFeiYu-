#include "host_state.hpp"
#include <string_view>

namespace dfy::plugins {
namespace {
// 在当前绑定的引用域保留来源，失败不会留下新输出。
dfy_v2_status DFY_V2_CALL retain(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return detail::boundary(error, [&](auto& host) { *output = detail::abi(host.references.retain(detail::native(source))); return DFY_V2_OK; });
}
// 仅释放当前域中的持有引用，跨实例或过期引用由正式注册表拒绝。
// 步骤：校验引用 → 对通知先归还原生客户端持有 → 释放边界引用。
dfy_v2_status DFY_V2_CALL release(dfy_v2_handle owned, dfy_v2_error* error) {
    return detail::boundary(error, [&](auto& host) {
        const auto handle = detail::native(owned);
        if (!handle.empty()) {
            const auto& object = host.references.get(handle).object;
            if (const auto* registration = std::get_if<std::shared_ptr<detail::NotificationReference>>(&object)) {
                host.notifications->release((*registration)->registration);
            }
        }
        host.references.release(handle);
        return DFY_V2_OK;
    });
}
const dfy_v2_reference_api reference_api = {{sizeof(dfy_v2_reference_api), 1}, retain, release};
}
// 保存服务状态，分配失败不形成可绑定的上下文。
HostServices::HostServices(std::size_t capacity, std::size_t reader_capacity)
    : state_(std::make_shared<detail::HostState>(capacity, reader_capacity)) {}
// 在已无资源责任时关闭空所有者；活动绑定保持状态，未完成清理不得以析构代替。
HostServices::~HostServices() {
    if (!state_->resources.size() && !state_->resources.incoming_uses() && !state_->resources.outgoing_uses()) {
        state_->resources.stop_accepting();
        state_->resources.finish_close();
    }
}
// 在当前线程建立嵌套绑定，拒绝错误线程后不改动绑定栈。
// 步骤：保持宿主状态 → 核对创建线程 → 保存并替换当前绑定。
HostServices::Binding::Binding(std::shared_ptr<detail::HostState> state) : state_(std::move(state)) {
    state_->require_owner();
    previous_ = std::exchange(detail::current_host, state_.get());
}
// 在成员释放前恢复外层绑定，不留下指向本次状态的线程指针。
HostServices::Binding::~Binding() { detail::current_host = previous_; }
// 返回不可移动的作用域绑定，覆盖一次原生调用或受控回调。
HostServices::Binding HostServices::bind() const { return Binding(state_); }
// 将非空宿主值登记为本实例引用，不调用插件代码。
// 步骤：核对线程与对象 → 建立引用记录 → 返回 C 句柄副本。
dfy_v2_handle HostServices::expose(std::shared_ptr<const runtime::Value> value, bool shareable) {
    state_->require_owner();
    if (!value) throw std::invalid_argument("host: value required");
    return detail::abi(state_->references.insert(std::make_shared<detail::HostReference>(detail::HostReference{std::move(value)}), shareable));
}
// 返回宿主线程可观察的引用计量。
std::size_t HostServices::references() const { state_->require_owner(); return state_->references.size(); }
// 登记非空描述快照，宿主独立保持名称及字段寿命。
dfy_v2_handle HostServices::expose(std::shared_ptr<const runtime::StructDescriptor> descriptor) {
    state_->require_owner();
    if (!descriptor) throw std::invalid_argument("host: descriptor required");
    return detail::abi(state_->references.insert(std::make_shared<detail::HostReference>(detail::HostReference{std::move(descriptor)})));
}
// 登记非空结构载荷，保持其字段与描述的宿主所有权。
dfy_v2_handle HostServices::expose(std::shared_ptr<const runtime::StructValue> value) {
    state_->require_owner();
    if (!value) throw std::invalid_argument("host: structure required");
    return detail::store_object(*state_, std::move(value));
}
// 在固定控制器预算中独立保持读取责任，先签发引用再提交非抛出持有。
// 步骤：检查线程与控制器 → 查找空槽并拒绝重复 → 签发引用 → 保存宿主所有权。
dfy_v2_handle HostServices::expose(std::shared_ptr<events::SubscriptionReader> reader) {
    state_->require_owner();
    if (!reader) throw std::invalid_argument("host: reader required");
    for (const auto& item : state_->readers) if (item.reader == reader) throw std::logic_error("host: reader already exposed");
    for (auto& item : state_->readers) {
        if (!item.resource.empty()) continue;
        item.resource = state_->resources.prepare();
        runtime::Handle exposed{};
        try {
            exposed = state_->references.insert(std::make_shared<detail::HostReference>(detail::HostReference{reader}), false);
            state_->resources.activate(item.resource);
            item.reader = std::move(reader);
            return detail::abi(exposed);
        } catch (...) {
            state_->references.release(exposed);
            state_->resources.begin_cleanup(item.resource); state_->resources.finish_cleanup(item.resource); item = {};
            throw;
        }
    }
    throw std::length_error("host: reader capacity exhausted");
}
// 显式推进宿主持有的读取，不内联调用 DLL 消费者。
void HostServices::advance() {
    state_->require_owner();
    for (auto& slot : state_->readers) {
        const auto reader = slot.reader;
        if (!reader) continue;
        reader->advance(); reader->collect();
        detail::finish_reader(*state_, reader);
    }
}
// 保存共享广播通道及读取预算，不自动创建接收方。
void HostServices::enable_events(std::shared_ptr<events::BroadcastChannel> channel, std::size_t request_capacity) {
    state_->require_owner();
    if (!channel || !request_capacity || state_->channel) throw std::invalid_argument("host: fresh channel and positive request capacity required");
    state_->channel = std::move(channel);
    state_->request_capacity = request_capacity;
}
// 在完整分配成功后绑定代码与通知池，不替换已有消费者入口。
// 步骤：核对线程及配置 → 构造通知池 → 转交代码持有和池。
void HostServices::enable_notifications(platform::CodeLease code, std::size_t capacity) {
    state_->require_owner();
    if (!code || state_->notifications) throw std::invalid_argument("host: live code and fresh notification configuration required");
    auto notifications = std::make_unique<runtime::CompletionNotifications>(capacity);
    state_->code = std::move(code);
    state_->notifications = std::move(notifications);
}
// 显式安排一项通知，未启用时没有待调度能力。
runtime::ScheduleResult HostServices::schedule_one(runtime::FifoExecutor& executor) {
    state_->require_owner();
    return state_->notifications ? state_->notifications->schedule_one(executor) : runtime::ScheduleResult::idle;
}
// 返回当前仍承担收尾责任的登记计量。
std::size_t HostServices::resources() const { state_->require_owner(); return state_->resources.size(); }
// 暴露清理故障供外层停止流程保留实例与代码。
bool HostServices::cleanup_failed() const { state_->require_owner(); return state_->cleanup_failed; }
// 在未进入消费者时停止接纳，逐项收尾并保留任何未完成责任。
// 步骤：拒绝活动回调 → 关闭接纳并撤销通知 → 取消关闭读取 → 检查清理结果。
bool HostServices::begin_shutdown() {
    state_->require_owner();
    if (state_->notifications && state_->notifications->running()) return false;
    state_->resources.stop_accepting();
    if (state_->notifications) state_->notifications->revoke_pending();
    for (auto& slot : state_->readers) {
        const auto reader = slot.reader;
        if (!reader) continue;
        try { reader->close(events::CloseMode::cancel); detail::finish_reader(*state_, reader); }
        catch (...) { state_->cleanup_failed = true; }
    }
    return !state_->cleanup_failed && !state_->resources.size();
}
// 在插件已不会访问服务后撤销所有残余引用，不把排队包装误认为还会进入 DLL。
// 步骤：核对收尾前提 → 销毁静止通知池 → 清空引用 → 关闭所有者并释放代码。
bool HostServices::finish_shutdown() {
    state_->require_owner();
    if (state_->resources.state() == runtime::OwnerState::open) throw std::logic_error("host: shutdown not started");
    if (state_->cleanup_failed || state_->resources.size() || state_->callback_pins ||
        (state_->notifications && state_->notifications->running())) return false;
    state_->notifications.reset();
    state_->references.clear();
    state_->resources.finish_close();
    state_->code.reset();
    return true;
}
// 统计本实例自身的代码持有，不包含外部能力或依赖方的凭证。
std::size_t HostServices::code_holds() const {
    state_->require_owner();
    return (state_->code ? 1 : 0) + state_->callback_pins;
}
// 只在当前绑定中提供实际实现的宿主服务，插件静态查询不承担此职责。
// 步骤：清空输出并校验参数 → 检查绑定 → 精确匹配名称与版本。
dfy_v2_status DFY_V2_CALL HostServices::query(dfy_v2_string_view name, uint32_t version,
    const void** output, dfy_v2_error* error) noexcept {
    if (output) *output = nullptr;
    if (error) error->length = 0;
    if (!output || !name.data || !name.length || !version) return DFY_V2_INVALID_ARGUMENT;
    return detail::boundary(error, [&](auto& host) {
        const std::string_view requested(name.data, name.length);
        if (version == 2) {
            if (requested == DFY_V2_TYPE_INTERFACE) *output = &detail::type_api_v2();
            else if (requested == DFY_V2_VALUE_INTERFACE) *output = &detail::value_api_v2();
            else if (requested == DFY_V2_STRUCT_INTERFACE) *output = &detail::struct_api_v2();
            else if (requested == DFY_V2_CONSTRAINT_INTERFACE) *output = &detail::constraint_api_v2();
            else return DFY_V2_UNSUPPORTED;
            return DFY_V2_OK;
        }
        if (version != 1) return DFY_V2_UNSUPPORTED;
        if (requested == DFY_V2_REFERENCE_INTERFACE) *output = &reference_api;
        else if (requested == DFY_V2_TYPE_INTERFACE) *output = &detail::type_api();
        else if (requested == DFY_V2_VALUE_INTERFACE) *output = &detail::value_api();
        else if (requested == DFY_V2_STRUCT_INTERFACE) *output = &detail::struct_api();
        else if (requested == DFY_V2_ARRAY_INTERFACE) *output = &detail::array_api();
        else if (requested == DFY_V2_MAP_INTERFACE) *output = &detail::map_api();
        else if (requested == DFY_V2_ENUM_INTERFACE) *output = &detail::enum_api();
        else if (requested == DFY_V2_CONSTRAINT_INTERFACE) *output = &detail::constraint_api();
        else if (requested == DFY_V2_UNION_INTERFACE) *output = &detail::union_api();
        else if (requested == DFY_V2_READER_INTERFACE) *output = &detail::reader_api();
        else if (requested == DFY_V2_OPERATION_STATE_INTERFACE) *output = &detail::operation_api();
        else if (requested == DFY_V2_OPERATION_CANCEL_INTERFACE) *output = &detail::cancel_api();
        else if (requested == DFY_V2_OPERATION_NOTIFY_INTERFACE && host.notifications) *output = &detail::notification_api();
        else if (requested == DFY_V2_SUBSCRIPTION_INTERFACE && host.channel) *output = &detail::subscription_api();
        else return DFY_V2_UNSUPPORTED;
        return DFY_V2_OK;
    });
}
}
