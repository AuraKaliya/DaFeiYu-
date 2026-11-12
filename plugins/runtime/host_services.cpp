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
HostServices::HostServices(std::size_t capacity) : state_(std::make_shared<detail::HostState>(capacity)) {}
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
// 只在当前绑定中提供实际实现的宿主服务，插件静态查询不承担此职责。
// 步骤：清空输出并校验参数 → 检查绑定 → 精确匹配名称与版本。
dfy_v2_status DFY_V2_CALL HostServices::query(dfy_v2_string_view name, uint32_t version,
    const void** output, dfy_v2_error* error) noexcept {
    if (output) *output = nullptr;
    if (error) error->length = 0;
    if (!output || !name.data || !name.length || !version) return DFY_V2_INVALID_ARGUMENT;
    return detail::boundary(error, [&](auto&) {
        if (std::string_view(name.data, name.length) != DFY_V2_REFERENCE_INTERFACE || version != 1) return DFY_V2_UNSUPPORTED;
        *output = &reference_api;
        return DFY_V2_OK;
    });
}
}
