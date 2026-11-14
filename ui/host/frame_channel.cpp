#include "frame_channel.hpp"
#include <nlohmann/json.hpp>

namespace dfy::ui {
using Json = nlohmann::json;
// 对已绑定 Frame 安装全部入口，事件回调只弱引用通道以避免引用环。
// 步骤：核对 Frame 能力 → 登记导航与消息 → 登记真实销毁 → 计入活动通道。
std::shared_ptr<FrameChannel> FrameChannel::attach(ICoreWebView2Frame* frame, std::uint64_t view, std::shared_ptr<FrameChecks> checks) {
    auto self = std::shared_ptr<FrameChannel>(new FrameChannel(view,std::move(checks)));
    require_hresult(frame->QueryInterface(IID_PPV_ARGS(&self->frame_)));
    const std::weak_ptr<FrameChannel> weak = self;
    // 每次文档导航都先裁定权限，取消的改源导航也永久撤销旧上下文。
    // 步骤：核对真实目标 → 撤销或激活 → 核对旧授权已失效 → 拒绝越界导航。
    const auto navigate = Callback<ICoreWebView2FrameNavigationStartingEventHandler>([weak](ICoreWebView2Frame*,ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
        const auto state = weak.lock(); bool allowed = false;
        if (state && state->active_ && state->frame_) {
            try { allowed = state->scope_.navigate(read_text([&](auto** uri) { return args->get_Uri(uri); })); }
            catch (...) { state->scope_.revoke(); }
            if (!allowed) {
                ++state->checks_->blocked_navigation;
                if (state->scope_.authorize(state->scope_.url(),state->scope_.id(),"probe.echo") == FrameDecision::inactive)
                    ++state->checks_->revoked_after_navigation;
            }
        }
        return args->put_Cancel(!allowed);
    });
    self->navigation_.bind(self->frame_.Get(),&ICoreWebView2Frame2::remove_NavigationStarting,[&](auto* token) { return self->frame_->add_NavigationStarting(navigate.Get(),token); });
    const auto message = Callback<ICoreWebView2FrameWebMessageReceivedEventHandler>([weak](ICoreWebView2Frame*,ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
        if (const auto state = weak.lock()) return state->message(args);
        return E_ABORT;
    });
    self->messages_.bind(self->frame_.Get(),&ICoreWebView2Frame2::remove_WebMessageReceived,[&](auto* token) { return self->frame_->add_WebMessageReceived(message.Get(),token); });
    const auto destroyed = Callback<ICoreWebView2FrameDestroyedEventHandler>([weak](ICoreWebView2Frame*,IUnknown*) -> HRESULT {
        if (const auto state = weak.lock()) { state->dead_ = true; ++state->checks_->destroyed; state->close(); }
        return S_OK;
    });
    self->destroyed_.bind(frame,&ICoreWebView2Frame::remove_Destroyed,[&](auto* token) { return frame->add_Destroyed(destroyed.Get(),token); });
    self->active_ = true; ++self->checks_->active; return self;
}
// 拒绝未登记字段和非法类型，只有实际来源及代次匹配时返回输入文本。
// 步骤：复制有界消息 → 校验固定结构 → 裁定来源与能力 → 发回非执行式 JSON。
HRESULT FrameChannel::message(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept {
    if (!active_ || dead_ || !frame_) return E_ABORT;
    try {
        const auto source = read_text([&](auto** value) { return args->get_Source(value); });
        const auto request = Json::parse(utf8(read_text([&](auto** value) { return args->get_WebMessageAsJson(value); })));
        bool valid = request.is_object() && request.size() == 4;
        for (const auto key : {"request","view","method","text"}) valid = valid && request.contains(key) && request[key].is_string();
        if (valid) valid = request["request"].get_ref<const std::string&>().size() <= 16 && request["view"].get_ref<const std::string&>().size() <= 20 &&
            request["method"].get_ref<const std::string&>().size() <= 32 && request["text"].get_ref<const std::string&>().size() <= 1024;
        Json reply{{"request",""},{"decision","malformed"},{"text",""}};
        if (request.is_object() && request.contains("request") && request["request"].is_string() && request["request"].get_ref<const std::string&>().size() <= 16) reply["request"] = request["request"];
        if (valid) {
            const auto decision = scope_.authorize(source,request["view"].get_ref<const std::string&>(),request["method"].get_ref<const std::string&>());
            const auto index = static_cast<std::size_t>(decision); ++checks_->decisions[index];
            constexpr const char* names[]{"allowed","inactive","wrong_source","stale_view","forbidden_method"};
            reply["decision"] = names[index]; if (decision == FrameDecision::allowed) reply["text"] = request["text"];
        } else ++checks_->malformed;
        return frame_->PostWebMessageAsJson(utf16(reply.dump()).c_str());
    } catch (...) { ++checks_->malformed; return E_INVALIDARG; }
}
// 先撤销调用权限，再归还事件与原生 Frame；真实销毁与主动解绑分别计数。
// 步骤：撤销权限 → 归还事件 → 释放接口 → 归还活动计数。
void FrameChannel::close() noexcept {
    scope_.revoke(); messages_.reset(dead_); navigation_.reset(dead_); destroyed_.reset(dead_); frame_.Reset();
    if (active_) { active_ = false; --checks_->active; }
}
}
