#include "probe_host.hpp"
#include <nlohmann/json.hpp>
#include <charconv>
#include <limits>

namespace dfy::ui {
namespace {
// 读取完整非零十进制身份，保留 64 位精度。
std::uint64_t run_id_part(const nlohmann::json& value) {
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t id = 0;
    const auto result = std::from_chars(text.data(),text.data()+text.size(),id);
    if (result.ec != std::errc{} || result.ptr != text.data()+text.size() || !id)
        throw std::invalid_argument("UI: invalid run identity");
    return id;
}

// 将原业务及实际退出状态转换为稳定的界面状态名。
std::string_view operation_state(runtime::OperationState state) noexcept {
    switch (state) {
    case runtime::OperationState::pending: return "pending";
    case runtime::OperationState::succeeded: return "succeeded";
    case runtime::OperationState::failed: return "failed";
    case runtime::OperationState::cancelled: return "cancelled";
    }
    return "unknown";
}

// 将窗口命令状态映射成固定标签，结果及实际退出来自独立的回合条目。
std::string_view dialogue_state(DialogueCommandState state) noexcept {
    using State = DialogueCommandState;
    switch (state) {
    case State::unavailable: return "unavailable";
    case State::queued: return "queued";
    case State::accepted: return "accepted";
    case State::busy: return "busy";
    case State::rejected: return "rejected";
    case State::conflict: return "conflict";
    case State::expired: return "expired";
    }
    return "rejected";
}

// 以十进制字符串返回原命令和正式回合身份。
nlohmann::json dialogue_receipt(const DialogueReceipt& receipt) {
    return {{"command",std::to_string(receipt.command)},{"state",dialogue_state(receipt.state)},
        {"session",std::to_string(receipt.run.session)},{"turn",std::to_string(receipt.run.sequence)}};
}

// 区分普通回合、等待读回和已裁定的读回证据。
nlohmann::json verification_state(const std::optional<app::VerificationReport>& report) {
    if (!report) return nullptr;
    if (!report->match) return "pending";
    switch (*report->match) {
    case agent::ObservationMatch::satisfied: return "satisfied";
    case agent::ObservationMatch::not_satisfied: return "not_satisfied";
    case agent::ObservationMatch::unknown: return "unknown";
    }
    return "unknown";
}
}

// 创建和消息入口都只弱引用宿主，部分登记失败由临时实例回滚。
// 步骤：持有浏览器 → 登记主页面消息 → 登记原生 Frame 创建 → 发布已准备宿主。
std::shared_ptr<ProbeHost> ProbeHost::attach(ICoreWebView2* view, std::function<void()> failure, ReadRun read_run, Dialogue dialogue) {
    auto self = std::shared_ptr<ProbeHost>(new ProbeHost(std::move(failure),std::move(read_run),std::move(dialogue))); self->view_ = view;
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
// 处理主文档的快照、对话命令或固定 Frame 申请。
// 步骤：核对原生来源及大小 → 按固定类型分发 → 为 Frame 申请签发一次票据。
HRESULT ProbeHost::message(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept {
    bool issued = false;
    try {
        const auto source = read_text([&](auto** value) { return args->get_Source(value); });
        if (!center_document(source) && !(dialogue_.read && main_panel_document(source))) return E_ACCESSDENIED;
        const auto body = read_text([&](auto** value) { return args->get_WebMessageAsJson(value); });
        if (body.size() > 6*app::dialogue_text_limit+512) return E_INVALIDARG;
        const auto message = nlohmann::json::parse(utf8(body));
        if (!message.is_object()) return E_INVALIDARG;
        const auto type = message.value("type",std::string());
        if (type == "state.read") return run_status_reply(message);
        if (type == "dialogue.read" || type == "dialogue.submit") return dialogue_reply(message,type,source);
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

// 复核实际页面后进入对话桥，变化时才发回正文快照。
// 步骤：核对活动来源与协议 → 交付命令或复制受信快照 → 复核当前来源并回发。
HRESULT ProbeHost::dialogue_reply(const nlohmann::json& request, std::string_view type, std::wstring_view source) {
    const auto active = [&] { return main_panel_document(read_text([&](auto** value) { return view_->get_Source(value); })); };
    if (!dialogue_.read || !dialogue_.submit || !main_panel_document(source) || !active()) return E_ACCESSDENIED;
    if (request.size() != 5 || request.at("version") != "1") return E_INVALIDARG;
    const auto& id = request.at("request").get_ref<const std::string&>();
    if (id.empty() || id.size() > 64) return E_INVALIDARG;

    nlohmann::json reply{{"version","1"},{"request",id}};
    if (type == "dialogue.submit") {
        const auto command = run_id_part(request.at("command"));
        auto receipt = dialogue_.submit(command,request.at("text").get<std::string>());
        reply["event"] = "dialogue.receipt";
        reply["receipt"] = dialogue_receipt(receipt);
    } else {
        const auto& after = request.at("revision").get_ref<const std::string&>();
        if (after.size() > 20) return E_INVALIDARG;
        const auto& command = request.at("command");
        const auto view = dialogue_.read(command == "" ? 0 : run_id_part(command));
        const auto revision = view.display ? std::to_string(view.display->revision) : std::string();
        nlohmann::json turns = nullptr;
        if (view.display && revision != after) {
            turns = nlohmann::json::array();
            for (const auto& turn : view.display->turns) {
                turns.push_back({{"session",std::to_string(turn.run.session)},{"turn",std::to_string(turn.run.sequence)},
                    {"prompt",turn.prompt},{"answer",turn.answer},{"result",operation_state(turn.result)},{"exit",operation_state(turn.exit)}});
            }
        }
        reply.update({{"event","dialogue.snapshot"},{"ready",bool(view.display)},{"accepting",view.accepting},
            {"latestCommand",std::to_string(view.latest_command)},{"revision",revision},{"turns",std::move(turns)},
            {"receipt",view.receipt ? dialogue_receipt(*view.receipt) : nlohmann::json(nullptr)}});
    }
    if (!active()) return E_ACCESSDENIED;
    return view_->PostWebMessageAsJson(utf16(reply.dump()).c_str());
}
// 从副本构造有界运行摘要，并在发回前复核当前 WebView 文档。
// 步骤：核对协议、请求及运行身份 → 复制并筛选快照 → 核对当前来源后发回 JSON。
HRESULT ProbeHost::run_status_reply(const nlohmann::json& request) {
    if ((request.size() != 4 && request.size() != 6) || request.at("version") != "1" || request.at("topic") != "run")
        return E_INVALIDARG;
    const auto& id = request.at("request").get_ref<const std::string&>();
    if (id.empty() || id.size() > 64) return E_INVALIDARG;
    std::optional<agent::RunId> expected;
    if (request.size() == 6) expected = agent::RunId{run_id_part(request.at("session")),run_id_part(request.at("turn"))};

    const auto snapshot = read_run_ ? read_run_() : std::nullopt;
    nlohmann::json run = nullptr;
    if (snapshot && (!expected || *expected == agent::RunId{snapshot->status.session,snapshot->status.turn})) {
        const auto& status = snapshot->status;
        run = {{"session",std::to_string(status.session)},{"turn",std::to_string(status.turn)},
            {"result",operation_state(status.result)},{"exit",operation_state(status.exit)},
            {"waiting_confirmations",status.waiting_confirmations},{"verification",verification_state(snapshot->verification)}};
    }
    const auto current = read_text([&](auto** value) { return view_->get_Source(value); });
    if (!center_document(current) && !(dialogue_.read && main_panel_document(current))) return E_ACCESSDENIED;
    const nlohmann::json reply{{"event","state.snapshot"},{"version","1"},{"topic","run"},{"request",id},
        {"available",static_cast<bool>(read_run_)},{"run",std::move(run)}};
    return view_->PostWebMessageAsJson(utf16(reply.dump()).c_str());
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
