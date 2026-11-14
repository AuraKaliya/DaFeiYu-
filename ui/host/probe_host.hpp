#pragma once
#include "frame_channel.hpp"
#include <functional>

namespace dfy::ui {
// U01 的固定主页面控制入口，只签发一个在途 Frame，不提供通用宿主调用。
class ProbeHost : public std::enable_shared_from_this<ProbeHost> {
  public:
    // 绑定原生主页面消息与 Frame 创建，回调始终属于当前 STA。
    static std::shared_ptr<ProbeHost> attach(ICoreWebView2* view, std::function<void()> failure);
    // 关闭全部 UI 入口及当前 Frame 持有，后台浏览器退出由窗口独立确认。
    void close() noexcept;
    // 提供原生事实的只读统计，统计不持有浏览器或页面。
    std::shared_ptr<const FrameChecks> checks() const noexcept { return checks_; }
    // 兜底归还事件，禁止留下浏览器到宿主的强引用环。
    ~ProbeHost() { close(); }
  private:
    // 准备尚未登记的探针宿主。
    explicit ProbeHost(std::function<void()> failure) : failure_(std::move(failure)) {}
    // 校验实际主文档，只允许申请新的固定验证页面。
    HRESULT message(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept;
    // 将具体 Frame 与待签发身份绑定，无票据的额外 Frame 明确失败。
    void created(ICoreWebView2Frame* frame);
    ComPtr<ICoreWebView2> view_;
    EventSlot<ICoreWebView2> messages_;
    EventSlot<ICoreWebView2_4> frames_;
    EventSlot<ICoreWebView2Frame> gone_;
    std::shared_ptr<FrameChecks> checks_ = std::make_shared<FrameChecks>();
    std::shared_ptr<FrameChannel> frame_;
    std::function<void()> failure_;
    std::uint64_t next_ = 0, ticket_ = 0;
    bool closed_ = false;
};
}
