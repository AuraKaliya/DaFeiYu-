#include "../paths.hpp"
#include "local_content.hpp"
#include "frame_channel.hpp"
#include "interaction_probe.hpp"
#include "probe_host.hpp"
#include <windows.h>
#include <objbase.h>
#include <wrl.h>
#include <WebView2.h>
#include <filesystem>
#include <memory>
#include <cstdio>
#include <chrono>
#include <stdexcept>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
constexpr wchar_t page[] = L"https://center.dafeiyu.invalid/index.html";
// 将失败 HRESULT 转为本地容器错误，不跨越 Windows 或 COM 回调边界传播。
void require(HRESULT status) { if (FAILED(status)) throw std::runtime_error("WebView2 HRESULT="+std::to_string(status)); }

struct Window : std::enable_shared_from_this<Window> {
    HWND handle = nullptr;
    std::filesystem::path assets, profile;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    std::shared_ptr<dfy::ui::LocalContent> content;
    std::shared_ptr<dfy::ui::ProbeHost> host;
    std::shared_ptr<const dfy::ui::FrameChecks> checks;
    std::shared_ptr<dfy::ui::InteractionProbe> interaction;
    std::chrono::steady_clock::time_point deadline;
    bool loaded = false;
    EventRegistrationToken navigation{};
    unsigned pending = 0;
    bool closing = false, smoke = false, interaction_mode = false, lifecycle_mode = false, navigation_bound = false;
    int result = 0;
    // 在窗口线程报告初始化失败，并延后关闭，避免嵌套浏览器回调。
    // 步骤：输出本地容器错误 → 固定失败结果 → 提交关闭消息。
    void fail(const char* reason = "UI initialization failed") noexcept {
        std::fprintf(stderr,"%s\n",reason); result = 1; PostMessageW(handle,WM_CLOSE,0,0);
    }
    // 随窗口客户区调整浏览器，不在最小化期间创建新的容器。
    void resize() noexcept {
        RECT bounds{};
        if (controller && GetClientRect(handle,&bounds)) controller->put_Bounds(bounds);
    }
    // 将所属窗口的键盘焦点交给浏览器控件，不切换其他应用或窗口。
    void focus() noexcept { if (controller) controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); }
    // 关闭控制器并等待未交回的创建回调，避免回调访问已销毁窗口。
    // 步骤：关闭新接纳并隐藏 → 撤销通知并关闭浏览器 → 等待创建完成 → 销毁原生窗口。
    void close() noexcept {
        closing = true; ShowWindow(handle,SW_HIDE);
        KillTimer(handle,1); if (host) host->close();
        if (interaction) interaction->close();
        if (view && navigation_bound) { view->remove_NavigationCompleted(navigation); navigation_bound = false; }
        if (content) content->close();
        if (controller) controller->Close();
        view.Reset(); controller.Reset();
        if (!pending) { environment.Reset(); DestroyWindow(handle); }
    }
    // 在控制器完成后配置本地资源与最小浏览设置，再异步导航。
    // 步骤：取得能力与设置 → 映射本地资源 → 登记导航结果 → 加载离线页面。
    void configure() {
        require(controller->get_CoreWebView2(&view));
        content = dfy::ui::LocalContent::attach(environment.Get(),view.Get(),assets);
        ComPtr<ICoreWebView2Settings> settings; require(view->get_Settings(&settings));
        require(settings->put_AreHostObjectsAllowed(FALSE)); require(settings->put_IsWebMessageEnabled(TRUE));
        const auto weak = weak_from_this();
        host = dfy::ui::ProbeHost::attach(view.Get(),[weak] { if (const auto self = weak.lock()) self->fail("Frame host failed"); });
        checks = host->checks();
        require(view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        // 仅观察本次导航是否完成，通过窗口消息安排自动验收退出。
        // 步骤：读取导航结果 → 失败时记录错误 → 按验收模式请求关闭。
        [weak](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            if (const auto self = weak.lock()) {
                BOOL success = FALSE;
                if (FAILED(args->get_IsSuccess(&success)) || !success) {
                    COREWEBVIEW2_WEB_ERROR_STATUS error{}; args->get_WebErrorStatus(&error);
                    const auto reason = "U01 main navigation error="+std::to_string(static_cast<int>(error)); self->fail(reason.c_str());
                }
                else self->loaded = true;
            }
            return S_OK;
        }).Get(),&navigation));
        navigation_bound = true; resize(); require(view->Navigate((std::wstring(page)+(smoke ? L"#verify" : L"")).c_str()));
        if (smoke || interaction_mode) { deadline = std::chrono::steady_clock::now()+std::chrono::seconds(20); SetTimer(handle,1,20,nullptr); }
    }
    // 有界等待真实异步消息，不以导航成功替代 Frame 授权验收。
    // 步骤：检查原生完成条件 → 成功请求关闭 → 超时记录边界并失败。
    void tick() noexcept {
        if (loaded && interaction_mode && !interaction) {
            try {
                const auto weak = weak_from_this();
                if (!lifecycle_mode) {
                    const auto foreground = GetForegroundWindow();
                    require(controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC));
                    if (foreground != handle && GetForegroundWindow() == handle) throw std::runtime_error("U01: focus probe activated its window");
                }
                interaction = std::make_shared<dfy::ui::InteractionProbe>(view.Get(),[weak](bool passed) {
                    if (const auto self = weak.lock()) {
                        if (!passed) self->result = 1;
                        else std::fprintf(stderr,"Interaction protocol checks passed: %zu\n",self->interaction->passed());
                        PostMessageW(self->handle,WM_CLOSE,0,0);
                    }
                });
                if (lifecycle_mode) interaction->start_lifecycle(checks);
                else interaction->start();
            } catch (const std::exception& error) { fail(error.what()); }
            catch (...) { fail(); }
        }
        if (interaction) interaction->advance();
        if (smoke && loaded && content->blocked_requests() && checks->verified()) PostMessageW(handle,WM_CLOSE,0,0);
        else if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr,"Frame checks: loaded=%d active=%u allowed=%u inactive=%u stale=%u forbidden=%u navigation=%u malformed=%u\n",
                loaded,checks->active,checks->decisions[0],checks->decisions[1],checks->decisions[3],checks->decisions[4],checks->blocked_navigation,checks->malformed);
            fail("U01: Frame verification timed out");
        }
    }
    // 提交控制器创建，回调在原 STA 中接收或关闭迟到的控制器。
    // 步骤：保留创建责任 → 提交异步创建 → 按关闭状态处理回调 → 配置或回收。
    void create_controller() {
        const auto weak = weak_from_this(); ++pending;
        const auto status = environment->CreateCoreWebView2Controller(handle,Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        // 接收创建结果前先归还回调责任，关闭中的窗口只回收控制器。
        // 步骤：核对宿主存活 → 处理创建结果 → 按关闭状态配置或释放 → 捕获本地异常。
        [weak](HRESULT status,ICoreWebView2Controller* created) -> HRESULT {
            if (const auto self = weak.lock()) {
                --self->pending;
                try {
                    require(status); if (!created) throw std::runtime_error("WebView2: no controller");
                    self->controller = created;
                    if (!self->closing) self->configure();
                } catch (const std::exception& error) { self->fail(error.what()); }
                catch (...) { self->fail(); }
                if (self->closing) self->close();
            } else if (created) created->Close();
            return S_OK;
        }).Get());
        if (FAILED(status)) { --pending; require(status); }
    }
    // 创建独立环境，配置路径与回调寿命由当前窗口拥有。
    // 步骤：检查离线入口 → 提交环境创建 → 接收环境或错误 → 按窗口状态继续或收尾。
    void start() {
        if (!std::filesystem::is_regular_file(assets/L"index.html")) throw std::runtime_error("UI assets missing");
        const auto weak = weak_from_this(); ++pending;
        const auto status = CreateCoreWebView2EnvironmentWithOptions(nullptr,profile.c_str(),nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            // 在创建回调返回前固定环境责任，关闭中的窗口不再创建控制器。
            // 步骤：核对宿主 → 接收环境 → 按关闭状态创建或回收 → 捕获本地错误。
            [weak](HRESULT status,ICoreWebView2Environment* created) -> HRESULT {
                if (const auto self = weak.lock()) {
                    --self->pending;
                    try {
                        require(status); if (!created) throw std::runtime_error("WebView2: no environment");
                        self->environment = created;
                        if (!self->closing) self->create_controller();
                    } catch (const std::exception& error) { self->fail(error.what()); }
                    catch (...) { self->fail(); }
                    if (self->closing) self->close();
                }
                return S_OK;
            }).Get());
        if (FAILED(status)) { --pending; require(status); }
    }
};
// 将原生窗口事件转交所属容器，窗口存活期间仅借用状态指针。
// 步骤：绑定创建参数 → 分派尺寸和关闭 → 在销毁时结束消息循环。
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message == WM_NCCREATE) SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    auto* state = reinterpret_cast<Window*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (state) {
        if (message == WM_SIZE) { state->resize(); return 0; }
        if (message == WM_SETFOCUS) { state->focus(); return 0; }
        if (message == WM_TIMER) { state->tick(); return 0; }
        if (message == WM_CLOSE) { state->close(); return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(state->result); return 0; }
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
// 在 STA 中运行独立离线容器，验证入口不依赖终端、模型或插件。
// 步骤：核对参数与运行库 → 创建窗口及环境 → 推进消息循环 → 释放 COM 公寓。
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    const bool smoke = __argc == 2 && std::wstring_view(__wargv[1]) == L"--smoke";
    const bool interaction_mode = __argc == 2 && std::wstring_view(__wargv[1]) == L"--interaction";
    const bool lifecycle_mode = __argc == 2 && std::wstring_view(__wargv[1]) == L"--lifecycle";
    if (__argc > 1 && !smoke && !interaction_mode && !lifecycle_mode) return 2;
    if (FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 3;
    int result = 1;
    try {
        LPWSTR raw = nullptr; const auto status = GetAvailableCoreWebView2BrowserVersionString(nullptr,&raw);
        std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> version(raw,&CoTaskMemFree);
        if (FAILED(status) || !version) {
            if (!smoke && !interaction_mode && !lifecycle_mode) MessageBoxW(nullptr,L"请安装 Microsoft Edge WebView2 Evergreen Runtime。",L"大肥鱼管理中心",MB_OK|MB_ICONERROR);
            CoUninitialize(); return 4;
        }
        auto state = std::make_shared<Window>(); state->smoke = smoke; state->interaction_mode = interaction_mode || lifecycle_mode; state->lifecycle_mode = lifecycle_mode;
        state->assets = dfy::app::executable_directory()/L"ui";
        state->profile = dfy::app::default_user_data()/L"ui-probe";
        WNDCLASSW kind{}; kind.lpfnWndProc = procedure; kind.hInstance = instance; kind.lpszClassName = L"DaFeiYuU01";
        kind.hCursor = LoadCursorW(nullptr,IDC_ARROW); if (!RegisterClassW(&kind)) throw std::runtime_error("window class failed");
        const DWORD extended = smoke || interaction_mode || lifecycle_mode ? WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW : 0;
        state->handle = CreateWindowExW(extended,kind.lpszClassName,L"大肥鱼管理中心 · U01 验证",WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,CW_USEDEFAULT,1100,760,nullptr,nullptr,instance,state.get());
        if (!state->handle) throw std::runtime_error("window creation failed");
        ShowWindow(state->handle,smoke || interaction_mode || lifecycle_mode ? SW_HIDE : show);
        try { state->start(); } catch (const std::exception& error) { state->fail(error.what()); }
        catch (...) { state->fail(); }
        MSG message{}; BOOL next;
        while ((next = GetMessageW(&message,nullptr,0,0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        result = next < 0 ? 1 : static_cast<int>(message.wParam);
    } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); result = 1; }
    catch (...) { result = 1; }
    CoUninitialize(); return result;
}
