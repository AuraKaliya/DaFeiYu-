#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace dfy::ui {
inline constexpr std::wstring_view center_url = L"https://center.dafeiyu.invalid/index.html";
// 仅供 U01 的固定本地来源，denied 来源用于真实未授权页面的反向验证。
bool local_resource(std::wstring_view uri) noexcept;
// 主页面允许同文档 hash 路由，但不接受改变路径或查询参数。
bool center_document(std::wstring_view uri) noexcept;
// 为宿主签发的非零视图生成唯一初始地址，不接受前端自报来源。
std::wstring probe_url(std::uint64_t view);
enum class FrameDecision { allowed, inactive, wrong_source, stale_view, forbidden_method };
// 一次导航的授权；实例固定，重新导航或销毁后不能再次激活。
class FrameScope {
  public:
    // 固定宿主视图身份和唯一初始地址。
    explicit FrameScope(std::uint64_t view);
    // 只允许首次导航到已签发地址，其余导航永久撤销当前授权。
    bool navigate(std::wstring_view uri) noexcept;
    // 撤销未来调用，不代表已接纳业务或浏览器资源已经退出。
    void revoke() noexcept { state_ = State::revoked; }
    // 依据宿主观察到的来源、视图身份和固定探针方法裁定调用。
    FrameDecision authorize(std::wstring_view source, std::string_view view, std::string_view method) const noexcept;
    // 返回发给页面的视图字符串，不暴露原生指针或 C ABI 句柄。
    const std::string& id() const noexcept { return id_; }
    // 返回宿主固定的初始地址。
    const std::wstring& url() const noexcept { return url_; }
  private:
    enum class State { pending, active, revoked };
    const std::string id_;
    const std::wstring url_;
    State state_ = State::pending;
};
}
