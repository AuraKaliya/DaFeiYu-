#pragma once
#include "webview_support.hpp"
#include <filesystem>

namespace dfy::ui {
// 受控本地页面来源与浏览器事件，U01 不提供网络资源回退。
class LocalContent : public std::enable_shared_from_this<LocalContent> {
  public:
    // 在浏览器导航前映射本地目录并安装导航、资源和弹窗边界。
    static std::shared_ptr<LocalContent> attach(ICoreWebView2Environment* environment, ICoreWebView2* view,
        const std::filesystem::path& assets, bool main_panel);
    // 撤销全部页面边界登记，实际浏览器关闭仍由窗口负责。
    void close() noexcept;
    // 返回实际由原生资源事件拒绝的外部请求数。
    unsigned blocked_requests() const noexcept { return blocked_; }
    // 释放事件登记与浏览器接口引用。
    ~LocalContent() { close(); }
  private:
    // 仅允许 attach 完整准备后发布实例。
    LocalContent() = default;
    // 对未登记资源直接提供本地拒绝响应，不交给网络层获取。
    HRESULT resource(ICoreWebView2WebResourceRequestedEventArgs* args) noexcept;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2> view_;
    EventSlot<ICoreWebView2> resources_, navigation_, popup_;
    unsigned blocked_ = 0;
};
}
