#include "frame_scope.hpp"
#include <stdexcept>

namespace dfy::ui {
namespace {
// 片段只标识同文档位置，不参与文档来源和宿主代次的比较。
std::wstring_view document_uri(std::wstring_view uri) noexcept { return uri.substr(0,uri.find(L'#')); }
}
// 只允许主入口文档及其内部 hash 路由。
bool center_document(std::wstring_view uri) noexcept { return document_uri(uri) == center_url; }
// 固定来源包含结尾斜线，拒绝同名前缀域、其他协议和未登记端口。
bool local_resource(std::wstring_view uri) noexcept {
    return uri.starts_with(L"https://center.dafeiyu.invalid/") ||
        uri.starts_with(L"https://probe.dafeiyu.invalid/") || uri.starts_with(L"https://denied.dafeiyu.invalid/");
}
// 生成含宿主代次的地址，使旧文档即使同源也不能复用新视图身份。
std::wstring probe_url(std::uint64_t view) {
    if (!view) throw std::invalid_argument("UI: nonzero view required");
    return L"https://probe.dafeiyu.invalid/frames/index.html?view="+std::to_wstring(view);
}
// 准备仅待首次导航的权限，不在构造时授予页面调用能力。
FrameScope::FrameScope(std::uint64_t view) : id_(std::to_string(view)), url_(probe_url(view)) {}
// 首次匹配导航激活授权，后续导航无论是否同地址都必须重新由宿主签发。
// 步骤：核对待激活状态和地址 → 激活或永久撤销 → 返回导航是否可接受。
bool FrameScope::navigate(std::wstring_view uri) noexcept {
    const bool allowed = state_ == State::pending && document_uri(uri) == url_;
    state_ = allowed ? State::active : State::revoked;
    return allowed;
}
// 身份与方法校验独立于页面自报角色，U01 仅允许无业务副作用的回显探针。
// 步骤：检查有效状态 → 核对实际文档来源 → 核对视图代次 → 检查固定方法。
FrameDecision FrameScope::authorize(std::wstring_view source, std::string_view view, std::string_view method) const noexcept {
    if (state_ != State::active) return FrameDecision::inactive;
    if (document_uri(source) != url_) return FrameDecision::wrong_source;
    if (view != id_) return FrameDecision::stale_view;
    if (method != "probe.echo") return FrameDecision::forbidden_method;
    return FrameDecision::allowed;
}
}
