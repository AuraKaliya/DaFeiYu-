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
dfy_v2_status DFY_V2_CALL release(dfy_v2_handle owned, dfy_v2_error* error) {
    return detail::boundary(error, [&](auto& host) { host.references.release(detail::native(owned)); return DFY_V2_OK; });
}
const dfy_v2_reference_api reference_api = {{sizeof(dfy_v2_reference_api), 1}, retain, release};
}
// 保存服务状态，分配失败不形成可绑定的上下文。
HostServices::HostServices(std::size_t capacity, std::size_t reader_capacity)
    : state_(std::make_shared<detail::HostState>(capacity, reader_capacity)) {}
// 默认释放控制器持有，活动绑定仍保持当前调用期的状态。
HostServices::~HostServices() = default;
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
    for (const auto& item : state_->readers) if (item == reader) throw std::logic_error("host: reader already exposed");
    for (auto& item : state_->readers) {
        if (item) continue;
        const auto handle = state_->references.insert(std::make_shared<detail::HostReference>(detail::HostReference{reader}), false);
        item = std::move(reader);
        return detail::abi(handle);
    }
    throw std::length_error("host: reader capacity exhausted");
}
// 显式推进宿主持有的读取，不内联调用 DLL 消费者。
void HostServices::advance() {
    state_->require_owner();
    for (const auto& reader : state_->readers) if (reader) { reader->advance(); reader->collect(); }
}
// 只在当前绑定中提供实际实现的宿主服务，插件静态查询不承担此职责。
// 步骤：清空输出并校验参数 → 检查绑定 → 精确匹配名称与版本。
dfy_v2_status DFY_V2_CALL HostServices::query(dfy_v2_string_view name, uint32_t version,
    const void** output, dfy_v2_error* error) noexcept {
    if (output) *output = nullptr;
    if (error) error->length = 0;
    if (!output || !name.data || !name.length || !version) return DFY_V2_INVALID_ARGUMENT;
    return detail::boundary(error, [&](auto&) {
        if (version != 1) return DFY_V2_UNSUPPORTED;
        const std::string_view requested(name.data, name.length);
        if (requested == DFY_V2_REFERENCE_INTERFACE) *output = &reference_api;
        else if (requested == DFY_V2_TYPE_INTERFACE) *output = &detail::type_api();
        else if (requested == DFY_V2_VALUE_INTERFACE) *output = &detail::value_api();
        else if (requested == DFY_V2_STRUCT_INTERFACE) *output = &detail::struct_api();
        else if (requested == DFY_V2_READER_INTERFACE) *output = &detail::reader_api();
        else if (requested == DFY_V2_OPERATION_STATE_INTERFACE) *output = &detail::operation_api();
        else if (requested == DFY_V2_OPERATION_CANCEL_INTERFACE) *output = &detail::cancel_api();
        else return DFY_V2_UNSUPPORTED;
        return DFY_V2_OK;
    });
}
}
