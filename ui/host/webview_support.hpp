#pragma once
#include <windows.h>
#include <objbase.h>
#include <wrl.h>
#include <WebView2.h>
#include <climits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dfy::ui {
inline constexpr std::size_t probe_text_limit = 16384;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
// 由外层回调捕获 COM 错误，禁止异常穿过 Windows 接口。
inline void require_hresult(HRESULT result) { if (FAILED(result)) throw std::runtime_error("UI HRESULT="+std::to_string(result)); }
// 将有界 UTF-16 消息完整转换为 UTF-8，拒绝非法代理项。
// 步骤：检查输入上限 → 计算输出长度 → 分配并完成转换。
inline std::string utf8(std::wstring_view value, std::size_t limit = probe_text_limit) {
    if (value.size() > limit || value.size() > INT_MAX) throw std::length_error("UI: UTF-16 input too large");
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if (!count) throw std::runtime_error("UI: invalid UTF-16");
    std::string result(count,'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count,nullptr,nullptr)) throw std::runtime_error("UI: UTF-8 conversion failed");
    return result;
}
// 将已校验的 UTF-8 JSON 转为浏览器消息，不拼接可执行脚本。
// 步骤：检查输入上限 → 计算输出长度 → 分配并完成转换。
inline std::wstring utf16(std::string_view value, std::size_t limit = probe_text_limit) {
    if (value.size() > limit || value.size() > INT_MAX) throw std::length_error("UI: UTF-8 input too large");
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if (!count) throw std::runtime_error("UI: invalid UTF-8");
    std::wstring result(count,L'\0');
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count)) throw std::runtime_error("UI: UTF-16 conversion failed");
    return result;
}
// 有界复制并归还 COM 字符串，所有成功和失败路径都释放调用方缓冲。
// 步骤：接收缓冲并固定释放责任 → 检查结果及长度 → 返回独立副本。
template<class Read>
std::wstring read_text(Read read, std::size_t limit = probe_text_limit) {
    LPWSTR raw = nullptr; const auto result = read(&raw);
    std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> text(raw,&CoTaskMemFree);
    require_hresult(result);
    if (!text || limit > INT_MAX) throw std::length_error("UI text outside limit");
    const auto length = wcsnlen_s(text.get(),limit+1);
    if (length > limit) throw std::length_error("UI text outside limit");
    return {text.get(),length};
}
// 在原所属线程撤销一个 COM 事件，释放来源引用；禁止复制登记责任。
template<class Source>
class EventSlot {
  public:
    using Remove = HRESULT (STDMETHODCALLTYPE Source::*)(EventRegistrationToken);
    // 默认没有登记或来源持有。
    EventSlot() = default;
    // 析构归还唯一事件登记。
    ~EventSlot() { reset(); }
    // 防止同一 token 被两个对象撤销。
    EventSlot(const EventSlot&) = delete;
    // 防止覆盖尚未撤销的登记。
    EventSlot& operator=(const EventSlot&) = delete;
    // 先固定来源与撤销入口，再接纳事件；失败不会留下已启用登记。
    // 步骤：归还旧登记 → 固定来源与撤销入口 → 接纳并标记新登记。
    template<class Add>
    void bind(Source* source, Remove remove, Add add) {
        reset(); source_ = source; remove_ = remove;
        require_hresult(add(&token_)); active_ = true;
    }
    // 撤销仍有效的来源；来源已报告销毁时只归还接口引用，不调用失效对象。
    void reset(bool destroyed = false) noexcept {
        if (active_ && !destroyed) (source_.Get()->*remove_)(token_);
        active_ = false; source_.Reset();
    }
  private:
    ComPtr<Source> source_;
    Remove remove_ = nullptr;
    EventRegistrationToken token_{};
    bool active_ = false;
};
}
