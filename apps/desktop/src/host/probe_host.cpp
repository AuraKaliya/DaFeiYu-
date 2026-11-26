#include "probe_host.hpp"
#include <nlohmann/json.hpp>
#include "management/protocol.hpp"
#include <limits>

namespace dfy::ui {
// 创建和消息入口都只弱引用宿主，部分登记失败由临时实例回滚。
// 步骤：持有浏览器 → 登记主页面消息 → 登记原生 Frame 创建 → 发布已准备宿主。
std::shared_ptr<ProbeHost> ProbeHost::attach(ICoreWebView2* view, std::function<void()> failure, bool panel, Request request) {
    auto self = std::shared_ptr<ProbeHost>(new ProbeHost(std::move(failure),panel,std::move(request))); self->view_ = view;
    const std::weak_ptr<ProbeHost> weak = self;
    const auto navigation = Callback<ICoreWebView2NavigationStartingEventHandler>([weak](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
        if (const auto state = weak.lock()) ++state->generation_;
        return S_OK;
    });
    self->navigations_.bind(view,&ICoreWebView2::remove_NavigationStarting,[&](auto* token) { return view->add_NavigationStarting(navigation.Get(),token); });
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
// 处理主文档的快照、对话命令或固定 Frame 申请。
// 步骤：核对原生来源及大小 → 按固定类型分发 → 为 Frame 申请签发一次票据。
HRESULT ProbeHost::message(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept {
    bool issued = false;
    try {
        const auto source = read_text([&](auto** value) { return args->get_Source(value); });
        const bool panel = panel_ && main_panel_document(source);
        if (!center_document(source) && !panel) return E_ACCESSDENIED;
        const auto limit = panel ? management::request_limit : probe_text_limit;
        const auto body = read_text([&](auto** value) { return args->get_WebMessageAsJson(value); },limit);
        const auto message = nlohmann::json::parse(utf8(body,limit));
        if (!message.is_object()) return E_INVALIDARG;
        const auto type = message.value("type",std::string());
        if (type=="host.stop" || type=="logs.read" || type=="logs.open") {
            if (!panel || message.size()!=3 || message.at("version")!="1") return E_ACCESSDENIED;
            const auto& id = message.at("request").get_ref<const std::string&>();
            if (id.empty() || id.size()>64) return E_INVALIDARG;
            return management_reply(message,source,false);
        }
        if (type == "messages.subscribe" || type == "messages.read" || type == "messages.receipt" ||
            type == "mailbox.list" || type == "mailbox.read") {
            if (!panel) return E_ACCESSDENIED;
            return management_reply(message,source,false);
        }
        if (type == "state.read" || type == "room.read" || type.starts_with("dialogue."))
            return management_reply(message,source,type == "state.read");
        if (message.size() != 1 || type != "frame.open") return E_INVALIDARG;
        if (ticket_ || checks_->active) return view_->PostWebMessageAsJson(L"{\"event\":\"frame.busy\"}");
        if (next_ == std::numeric_limits<std::uint64_t>::max()) return E_FAIL;
        ticket_ = ++next_; issued = true;
        const nlohmann::json reply{{"event","frame.opened"},{"url",utf8(probe_url(ticket_))}};
        const auto status = view_->PostWebMessageAsJson(utf16(reply.dump()).c_str());
        if (FAILED(status)) ticket_ = 0;
        return status;
    } catch (...) { if (issued) ticket_ = 0; return E_INVALIDARG; }
}

// 按实际来源授予公开或主会话读写，业务分派不持有 WebView2。
// 步骤：核对发送文档与当前文档 → 分派到本体 → 复核文档后交付值副本。
HRESULT ProbeHost::management_reply(const nlohmann::json& request, std::wstring_view source, bool public_read) {
    const bool from_panel = panel_ && main_panel_document(source);
    if (!from_panel && (!public_read || !center_document(source))) return E_ACCESSDENIED;
    const auto current = read_text([&](auto** value) { return view_->get_Source(value); });
    if (!(from_panel ? main_panel_document(current) : center_document(current))) return E_ACCESSDENIED;

    // 异步回包只交给原始文档代次，即使刷新后使用相同 URL 也须重新查询。
    // 步骤：核对窗口和文档代次 → 从原生浏览器复核来源 → 回发有界消息。
    const auto deliver = [weak=weak_from_this(),epoch=generation_,from_panel,public_read](const nlohmann::json& reply) {
        const auto self = weak.lock();
        if (!self || self->closed_ || self->generation_!=epoch) return;
        try {
            const auto url = read_text([&](auto** out) { return self->view_->get_Source(out); });
            if (!(from_panel ? main_panel_document(url) : center_document(url))) return;
            if (from_panel && self->confirm_exit_ && reply.value("event", "") == "dialogue.snapshot" && reply.value("ready", false)) {
                auto confirmation = reply;
                confirmation["confirmStop"] = true;
                require_hresult(self->view_->PostWebMessageAsJson(utf16(confirmation.dump(),management::response_limit).c_str()));
                self->confirm_exit_ = false;
            } else {
                require_hresult(self->view_->PostWebMessageAsJson(utf16(reply.dump(),public_read ? probe_text_limit : management::response_limit).c_str()));
            }
        } catch (...) { self->failure_(); }
    };
    if (!request_(request,deliver) && request.contains("request") && request.at("request").is_string()) {
        if (request.contains("command") && request.at("type")!="dialogue.read")
            deliver({{"event","dialogue.receipt"},{"version","1"},{"request",request.at("request")},
                {"receipt",{{"command",request.at("command")},{"state","busy"}}}});
        else if (request.at("type")=="host.stop" || request.at("type")=="logs.open" || request.at("type")=="logs.read" ||
            request.value("type",std::string{}).starts_with("messages.") ||
            request.at("type")=="mailbox.list" || request.at("type")=="mailbox.read")
            deliver({{"event","management.error"},{"version","1"},{"request",request.at("request")},{"reason","management client queue unavailable"}});
    }
    return S_OK;
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
    closed_ = true; ticket_ = 0; messages_.reset(); navigations_.reset(); frames_.reset(); gone_.reset();
    if (frame_) frame_->close();
    frame_.reset(); view_.Reset();
}
}
