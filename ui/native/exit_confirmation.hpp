#pragma once
#include <windows.h>
#include <functional>
#include <utility>

namespace dfy::ui {
// 缺少配套面板时由托盘线程呈现确认框，外部退出仍能随消息循环一起收尾。
class ExitConfirmation {
  public:
    // 保存只作用于原本体的停止回调。
    explicit ExitConfirmation(std::function<void()> accept) : accept_(std::move(accept)) {}
    // 在创建线程关闭仍未作答的确认框。
    ~ExitConfirmation();
    // 对话框绑定原对象地址，不能复制其所有权。
    ExitConfirmation(const ExitConfirmation&) = delete;
    // 已创建的确认框必须始终对应原停止回调。
    ExitConfirmation& operator=(const ExitConfirmation&) = delete;
    // 同一时间只显示一个确认框，重复请求唤回它。
    DWORD show(HWND owner);
    // 在托盘消息循环内处理确认框的 Tab、Enter 和 Esc。
    bool translate(MSG& message) const { return dialog_ && IsDialogMessageW(dialog_, &message); }
  private:
    // 仅确认按钮能提交原本体的退出，取消和关窗都只回收对话框。
    static INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam);
    HWND dialog_ = nullptr;
    std::function<void()> accept_;
};
}
