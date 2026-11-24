#pragma once
#include "dialogue_channel.hpp"
#include "frame_channel.hpp"
#include "room_location.hpp"
#include "application.hpp"
#include <functional>
#include <nlohmann/json_fwd.hpp>

namespace dfy::ui {
// 主文档的固定消息入口，分发容器验证和宿主只读查询。
class ProbeHost : public std::enable_shared_from_this<ProbeHost> {
  public:
    using ReadRun = std::function<std::optional<app::RunStatusSnapshot>()>;
    using ReadRoom = std::function<std::optional<RoomLocation>()>;
    struct Dialogue {
        std::function<DialogueView(std::uint64_t)> read;
        std::function<DialogueReceipt(DialogueCommand)> submit;
    };
    // 绑定原生主页面消息与 Frame 创建，回调始终属于当前 STA。
    static std::shared_ptr<ProbeHost> attach(ICoreWebView2* view, std::function<void()> failure, ReadRun read_run = {}, Dialogue dialogue = {}, ReadRoom read_room = {});
    // 关闭全部 UI 入口及当前 Frame 持有，后台浏览器退出由窗口独立确认。
    void close() noexcept;
    // 提供原生事实的只读统计，统计不持有浏览器或页面。
    std::shared_ptr<const FrameChecks> checks() const noexcept { return checks_; }
    // 兜底归还事件，禁止留下浏览器到宿主的强引用环。
    ~ProbeHost() { close(); }
  private:
    // 准备尚未登记的探针宿主。
    explicit ProbeHost(std::function<void()> failure, ReadRun read_run, Dialogue dialogue, ReadRoom read_room)
        : failure_(std::move(failure)), read_run_(std::move(read_run)), dialogue_(std::move(dialogue)), read_room_(std::move(read_room)) {}
    // 核对实际主文档并分发已允许的固定请求。
    HRESULT message(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept;
    // 发送与原请求及可选运行身份匹配的只读状态。
    HRESULT run_status_reply(const nlohmann::json& request);
    // 为当前可信主面板处理正文查询和绑定原回合的会话操作。
    HRESULT dialogue_reply(const nlohmann::json& request, std::string_view type, std::wstring_view source);
    // 仅向实际可信主文档返回本体的位置摘要。
    HRESULT room_reply(const nlohmann::json& request, std::wstring_view source);
    // 将具体 Frame 与待签发身份绑定，无票据的额外 Frame 明确失败。
    void created(ICoreWebView2Frame* frame);
    ComPtr<ICoreWebView2> view_;
    EventSlot<ICoreWebView2> messages_;
    EventSlot<ICoreWebView2_4> frames_;
    EventSlot<ICoreWebView2Frame> gone_;
    std::shared_ptr<FrameChecks> checks_ = std::make_shared<FrameChecks>();
    std::shared_ptr<FrameChannel> frame_;
    std::function<void()> failure_;
    ReadRun read_run_;
    Dialogue dialogue_;
    ReadRoom read_room_;
    std::uint64_t next_ = 0, ticket_ = 0;
    bool closed_ = false;
};
}
