#include "probe_host.hpp"
#include <nlohmann/json.hpp>
#include <limits>

namespace dfy::ui {
// 创建和消息入口都只弱引用宿主，部分登记失败由临时实例回滚。
// 步骤：持有浏览器 → 登记主页面消息 → 登记原生 Frame 创建 → 发布已准备宿主。
std::shared_ptr<ProbeHost> ProbeHost::attach(ICoreWebView2* view, std::function<void()> failure) {
    auto self = std::shared_ptr<ProbeHost>(new ProbeHost(std::move(failure))); self->view_ = view;
    const std::weak_ptr<ProbeHost> weak = self;
    const auto message = Callback<ICoreWebView2WebMessageReceivedEventHandler>([weak](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
        if (const auto state = weak.lock(); state && !state->closed_) return state->message(args);
        return E_ABORT;
    });
    self->messages_.bind(view,&ICoreWebView2::remove_WebMessageReceived,[&](auto* token) { return view->add_WebMessageReceived(message.Get(),token); });
    ComPtr<ICoreWebView2_4> source; require_hresult(view->QueryInterface(IID_PPV_ARGS(&source)));
    // 只有原生 Frame 创建事件能完成绑定，页面不能自报一个 Frame 身份。
    // 步骤：读取实际 Frame → 完成单次绑定 → 隔离创建异常。
    const auto created = Callback<ICoreWebView2FrameCreatedEventHandler>([weak](ICoreWebView2*,ICoreWebView2FrameCreatedEventArgs* args) -> HRESULT {
        if (const auto state = weak.lock(); state && !state->closed_) {
            try { ComPtr<ICoreWebView2Frame> frame; require_hresult(args->get_Frame(&frame)); state->created(frame.Get()); }
            catch (...) { state->failure_(); }
        }
        return S_OK;
    });
    self->frames_.bind(source.Get(),&ICoreWebView2_4::remove_FrameCreated,[&](auto* token) { return source->add_FrameCreated(created.Get(),token); });
    return self;
}
// 只有主页面的固定申请可以取得新地址，正在运行或尚未绑定时拒绝重复申请。
// 步骤：核对实际来源及消息 → 检查活动与序号 → 签发一次票据 → 发回地址。
HRESULT ProbeHost::message(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept {
    bool issued = false;
    try {
        if (!center_document(read_text([&](auto** value) { return args->get_Source(value); }))) return E_ACCESSDENIED;
        const auto message = nlohmann::json::parse(utf8(read_text([&](auto** value) { return args->get_WebMessageAsJson(value); })));
        if (!message.is_object() || message.size() != 1 || message.value("type",std::string()) != "frame.open") return E_INVALIDARG;
        if (ticket_ || checks_->active) return view_->PostWebMessageAsJson(L"{\"event\":\"frame.busy\"}");
        if (next_ == std::numeric_limits<std::uint64_t>::max()) return E_FAIL;
        ticket_ = ++next_; issued = true;
        const nlohmann::json reply{{"event","frame.opened"},{"url",utf8(probe_url(ticket_))}};
        const auto status = view_->PostWebMessageAsJson(utf16(reply.dump()).c_str());
        if (FAILED(status)) ticket_ = 0;
        return status;
    } catch (...) { if (issued) ticket_ = 0; return E_INVALIDARG; }
}
// 归还旧对象后绑定新代次，真实销毁确认才通知主页面允许重新打开。
// 步骤：核对票据与活动数 → 建立授权通道 → 登记销毁确认 → 消费票据。
void ProbeHost::created(ICoreWebView2Frame* frame) {
    if (!ticket_ || checks_->active) throw std::logic_error("U01: frame without host ticket");
    frame_.reset(); gone_.reset();
    frame_ = FrameChannel::attach(frame,ticket_,checks_);
    const auto weak = weak_from_this();
    // 真实销毁发生后归还宿主持有，并让主页面解除重建等待。
    // 步骤：失效销毁登记 → 归还通道对象 → 提交关闭确认。
    const auto destroyed = Callback<ICoreWebView2FrameDestroyedEventHandler>([weak](ICoreWebView2Frame*,IUnknown*) -> HRESULT {
        if (const auto state = weak.lock(); state && !state->closed_) {
            state->gone_.reset(true); state->frame_.reset();
            return state->view_->PostWebMessageAsJson(L"{\"event\":\"frame.closed\"}");
        }
        return S_OK;
    });
    gone_.bind(frame,&ICoreWebView2Frame::remove_Destroyed,[&](auto* token) { return frame->add_Destroyed(destroyed.Get(),token); });
    ticket_ = 0;
}
// 先关闭接纳和事件，再释放当前授权对象；值统计可独立保留。
// 步骤：停止接纳并撤销登记 → 归还 Frame 通道 → 释放浏览器引用。
void ProbeHost::close() noexcept {
    closed_ = true; ticket_ = 0; messages_.reset(); frames_.reset(); gone_.reset();
    if (frame_) frame_->close();
    frame_.reset(); view_.Reset();
}
}
