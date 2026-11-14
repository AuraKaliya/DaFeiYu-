#pragma once
#include "frame_scope.hpp"
#include "webview_support.hpp"
#include <array>

namespace dfy::ui {
struct FrameChecks {
    std::array<unsigned,5> decisions{};
    unsigned active = 0, destroyed = 0, blocked_navigation = 0, malformed = 0, revoked_after_navigation = 0;
    // 核对真实消息和导航反向用例，不以页面自报成功代替原生计数。
    bool verified() const noexcept {
        return decisions[0] && decisions[3] && decisions[4] && blocked_navigation && malformed && revoked_after_navigation;
    }
};
// 绑定一个具体原生 Frame，前端声明不能替换绑定来源或代次。
class FrameChannel : public std::enable_shared_from_this<FrameChannel> {
  public:
    // 安装文档导航、消息和销毁监听，失败时撤销已安装部分。
    static std::shared_ptr<FrameChannel> attach(ICoreWebView2Frame* frame, std::uint64_t view, std::shared_ptr<FrameChecks> checks);
    // 撤销授权并归还事件引用；已销毁 Frame 不再调用其接口。
    void close() noexcept;
    // 最后持有者释放时归还当前通道。
    ~FrameChannel() { close(); }
  private:
    // 固定宿主签发视图和统计，不授予尚未导航的页面。
    FrameChannel(std::uint64_t view, std::shared_ptr<FrameChecks> checks) : scope_(view), checks_(std::move(checks)) {}
    // 从实际事件来源校验有界 JSON 并返回固定格式回显或拒绝。
    HRESULT message(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept;
    FrameScope scope_;
    std::shared_ptr<FrameChecks> checks_;
    ComPtr<ICoreWebView2Frame2> frame_;
    EventSlot<ICoreWebView2Frame2> messages_, navigation_;
    EventSlot<ICoreWebView2Frame> destroyed_;
    bool active_ = false, dead_ = false;
};
}
