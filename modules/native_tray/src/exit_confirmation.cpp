#include "exit_confirmation.hpp"
#include "../resources/tray_resource.h"

namespace dfy::ui {
// 外部入口已使本体退出时，未确认的用户请求随本窗口释放。
ExitConfirmation::~ExitConfirmation() { if (dialog_) DestroyWindow(dialog_); }

// 在托盘所属线程创建可取消的确认框，保持本体与托盘消息循环运行。
DWORD ExitConfirmation::show(HWND owner) {
    if (!dialog_ && !CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TRAY_EXIT), owner,
        procedure, reinterpret_cast<LPARAM>(this))) return GetLastError();
    ShowWindow(dialog_, SW_SHOWNORMAL);
    SetForegroundWindow(dialog_);
    return ERROR_SUCCESS;
}

// 将默认焦点放在继续运行，择一处理确认、取消和外部销毁。
// 步骤：绑定对话框与默认焦点 → 按明确选择分派 → 销毁时解除绑定。
INT_PTR CALLBACK ExitConfirmation::procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        auto* self = reinterpret_cast<ExitConfirmation*>(lparam);
        self->dialog_ = dialog;
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        SetFocus(GetDlgItem(dialog, IDCANCEL));
        return FALSE;
    }
    auto* self = reinterpret_cast<ExitConfirmation*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!self) return FALSE;
    if (message == WM_COMMAND && LOWORD(wparam) == IDYES) {
        DestroyWindow(dialog);
        self->accept_();
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
        DestroyWindow(dialog);
        return TRUE;
    }
    if (message == WM_NCDESTROY) self->dialog_ = nullptr;
    return FALSE;
}
}
