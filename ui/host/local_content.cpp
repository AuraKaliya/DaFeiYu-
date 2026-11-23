#include "local_content.hpp"
#include "frame_scope.hpp"

namespace dfy::ui {
// 导航前完成映射及事件登记，任一步失败由临时实例回收此前登记。
// 步骤：持有环境和页面 → 固定本地来源 → 登记资源及导航约束 → 返回已准备实例。
std::shared_ptr<LocalContent> LocalContent::attach(ICoreWebView2Environment* environment, ICoreWebView2* view,
    const std::filesystem::path& assets, bool main_panel) {
    auto self = std::shared_ptr<LocalContent>(new LocalContent()); self->environment_ = environment; self->view_ = view;
    ComPtr<ICoreWebView2_3> mapping; require_hresult(view->QueryInterface(IID_PPV_ARGS(&mapping)));
    require_hresult(mapping->SetVirtualHostNameToFolderMapping(L"center.dafeiyu.invalid",assets.c_str(),COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS));
    for (const auto host : {L"probe.dafeiyu.invalid",L"denied.dafeiyu.invalid"})
        require_hresult(mapping->SetVirtualHostNameToFolderMapping(host,assets.c_str(),COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS));
    require_hresult(view->AddWebResourceRequestedFilter(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));

    const std::weak_ptr<LocalContent> weak = self;
    const auto resource = Callback<ICoreWebView2WebResourceRequestedEventHandler>([weak](ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
        if (const auto state = weak.lock()) return state->resource(args);
        return E_ABORT;
    });
    self->resources_.bind(view,&ICoreWebView2::remove_WebResourceRequested,[&](auto* token) { return view->add_WebResourceRequested(resource.Get(),token); });
    // 按原生观察到的导航地址裁定主文档，读取失败保持拒绝。
    // 步骤：读取并核对主入口 → 将允许或拒绝写回导航参数。
    const auto navigation = Callback<ICoreWebView2NavigationStartingEventHandler>([main_panel](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
        bool allowed = false;
        try {
            const auto uri = read_text([&](auto** value) { return args->get_Uri(value); });
            allowed = center_document(uri) || (main_panel && main_panel_document(uri));
        } catch (...) {}
        return args->put_Cancel(!allowed);
    });
    self->navigation_.bind(view,&ICoreWebView2::remove_NavigationStarting,[&](auto* token) { return view->add_NavigationStarting(navigation.Get(),token); });
    const auto popup = Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* args) { return args->put_Handled(TRUE); });
    self->popup_.bind(view,&ICoreWebView2::remove_NewWindowRequested,[&](auto* token) { return view->add_NewWindowRequested(popup.Get(),token); });
    return self;
}
// 未登记来源或读取失败均返回拒绝响应，避免异常变成网络回退。
// 步骤：读取真实请求地址 → 放行固定本地来源 → 为其余请求构造本地 403。
HRESULT LocalContent::resource(ICoreWebView2WebResourceRequestedEventArgs* args) noexcept {
    try {
        ComPtr<ICoreWebView2WebResourceRequest> request; require_hresult(args->get_Request(&request));
        if (local_resource(read_text([&](auto** uri) { return request->get_Uri(uri); }))) return S_OK;
    } catch (...) {}
    ++blocked_;
    ComPtr<ICoreWebView2WebResourceResponse> response;
    const auto result = environment_->CreateWebResourceResponse(nullptr,403,L"Blocked",L"Content-Type: text/plain",&response);
    return FAILED(result) ? result : args->put_Response(response.Get());
}
// 先撤销入口和筛选器，再释放环境引用，重复关闭保持无副作用。
// 步骤：撤销事件 → 移除请求筛选器 → 释放页面与环境引用。
void LocalContent::close() noexcept {
    popup_.reset(); navigation_.reset(); resources_.reset();
    if (view_) view_->RemoveWebResourceRequestedFilter(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    view_.Reset(); environment_.Reset();
}
}
