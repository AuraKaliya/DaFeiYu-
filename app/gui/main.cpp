#include "../paths.hpp"
#include "local_content.hpp"
#include "frame_channel.hpp"
#include "interaction_probe.hpp"
#include "probe_host.hpp"
#include "browser_lifetime.hpp"
#include "run_status_host.hpp"
#ifdef DFY_UI_RUN_STATUS_TEST
#include "ui_main_panel_fixture.hpp"
#include "ui_run_status_probe.hpp"
#endif
#include <windows.h>
#include <dwmapi.h>
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
    std::filesystem::path assets;
    std::filesystem::path model_config;
    dfy::ui::ProbeProfile profile;
    dfy::ui::BrowserProcesses processes;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    std::shared_ptr<dfy::ui::LocalContent> content;
    std::shared_ptr<dfy::ui::ProbeHost> host;
    std::shared_ptr<const dfy::ui::FrameChecks> checks;
    std::shared_ptr<dfy::ui::InteractionProbe> interaction;
    std::unique_ptr<dfy::ui::RunStatusHost> run_status_host;
#ifdef DFY_UI_RUN_STATUS_TEST
    std::shared_ptr<dfy::tests::UiRunStatusProbe> run_status_probe;
    std::shared_ptr<dfy::tests::UiDialogueGate> panel_gate;
#endif
    std::chrono::steady_clock::time_point deadline;
    std::chrono::steady_clock::time_point close_deadline;
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool loaded = false;
    long long navigation_ms = -1;
    EventRegistrationToken navigation{};
    unsigned pending = 0, pending_at_close = 0;
    bool closing = false, smoke = false, interaction_mode = false, lifecycle_mode = false, early = false, close_controller = false, navigation_bound = false;
    bool finish_posted = false, process_check_failed = false, close_warned = false;
    bool offline = false, offline_enabled = false;
    bool business_mode = false;
    bool room_preview = false, room_busy = false;
    bool main_panel = false, main_panel_check = false;
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
    // 关闭入口和控制器后继续推进消息，直到创建回调及浏览器进程实际结束。
    // 步骤：停止接纳 → 记录所属进程 → 关闭浏览器接口 → 延后确认退出。
    void close() noexcept {
        if (!closing) { closing = true; pending_at_close = pending; close_deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10); }
        ShowWindow(handle,SW_HIDE);
        KillTimer(handle,1); if (host) host->close();
        if (interaction) interaction->close();
        if (run_status_host) run_status_host->close();
        if (environment) {
            try { processes.observe(environment.Get()); }
            catch (const std::exception& error) { process_check_failed = true; result = 1; std::fprintf(stderr,"%s\n",error.what()); }
        }
        if (view && navigation_bound) { view->remove_NavigationCompleted(navigation); navigation_bound = false; }
        if (controller) controller->Close();
        if (content) content->close();
        view.Reset(); controller.Reset();
        // 尚未交回的控制器可能继续创建进程，保留环境供最后一次回调重新观察。
        if (!pending) environment.Reset();
        SetTimer(handle,2,20,nullptr);
    }
    // 用内核退出、回调和目录回收共同确认关闭，不在 COM 回调栈内销毁窗口。
    // 步骤：等待全部责任 → 清理本次配置 → 延后销毁窗口或报告超时。
    void finish_close() noexcept {
        const bool callbacks_done = !pending && (!interaction || !interaction->pending());
        const bool frames_done = !checks || (!checks->active && !checks->ready && !checks->live_channels);
        const bool business_done = !run_status_host || run_status_host->exited();
        if (business_done && run_status_host && run_status_host->failed()) result = 1;
        if (callbacks_done && frames_done && business_done && processes.exited() && !process_check_failed && profile.cleanup()) {
            if (!finish_posted) { finish_posted = true; PostMessageW(handle,WM_APP+1,0,0); }
        } else if (std::chrono::steady_clock::now() >= close_deadline) {
            result = 1;
            if (!close_warned) {
                close_warned = true;
                std::fprintf(stderr,"UI: shutdown incomplete; profile retained (callbacks=%d frames=%d application=%d browsers=%d)\n",
                    callbacks_done,frames_done,business_done,processes.exited());
            }
            if (callbacks_done && business_done && processes.exited() && !finish_posted) { finish_posted = true; PostMessageW(handle,WM_APP+1,0,0); }
        }
    }
    // 在控制器完成后配置本地资源与最小浏览设置，再异步导航。
    // 步骤：取得能力与设置 → 映射本地资源 → 登记导航结果 → 加载离线页面。
    void configure() {
        require(controller->get_CoreWebView2(&view));
        content = dfy::ui::LocalContent::attach(environment.Get(),view.Get(),assets,main_panel);
        ComPtr<ICoreWebView2Settings> settings; require(view->get_Settings(&settings));
        require(settings->put_AreHostObjectsAllowed(FALSE)); require(settings->put_IsWebMessageEnabled(!room_preview));
        const auto weak = weak_from_this();
        dfy::ui::ProbeHost::ReadRun read_run;
        if (run_status_host) read_run = [weak] {
            if (const auto self = weak.lock()) return self->run_status_host->snapshot();
            return std::optional<dfy::app::RunStatusSnapshot>{};
        };
        dfy::ui::ProbeHost::Dialogue dialogue;
        dfy::ui::ProbeHost::ReadRoom read_room;
        if (main_panel) {
            read_room = [weak]() -> std::optional<dfy::ui::RoomLocation> {
                if (const auto self = weak.lock(); self && !self->closing) return self->run_status_host->room_location();
                return {};
            };
            dialogue.read = [weak](auto command) -> dfy::ui::DialogueView {
                if (const auto self = weak.lock(); self && !self->closing) return self->run_status_host->dialogue(command);
                return {};
            };
            dialogue.submit = [weak](dfy::ui::DialogueCommand command) -> dfy::ui::DialogueReceipt {
                if (const auto self = weak.lock(); self && !self->closing) return self->run_status_host->submit_dialogue(std::move(command));
                return {command.id,dfy::ui::DialogueCommandState::unavailable,{}};
            };
        }
        host = dfy::ui::ProbeHost::attach(view.Get(),[weak] { if (const auto self = weak.lock()) self->fail("Frame host failed"); },
            std::move(read_run),std::move(dialogue),std::move(read_room));
        checks = host->checks();
        require(view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        // 处理主文档导航回执，自动验收由计时器另行推进。
        // 步骤：读取结果 → 按成功或失败记录就绪时刻或提交错误关闭。
        [weak](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            if (const auto self = weak.lock(); self && !self->closing) {
                BOOL success = FALSE;
                if (FAILED(args->get_IsSuccess(&success)) || !success) {
                    COREWEBVIEW2_WEB_ERROR_STATUS error{}; args->get_WebErrorStatus(&error);
                    const auto reason = "U01 main navigation error="+std::to_string(static_cast<int>(error)); self->fail(reason.c_str());
                }
                else { self->loaded = true; self->navigation_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-self->started).count(); }
            }
            return S_OK;
        }).Get(),&navigation));
        navigation_bound = true; resize();
        if (smoke || interaction_mode) deadline = std::chrono::steady_clock::now()+std::chrono::seconds(20);
        if (smoke || interaction_mode || run_status_host) SetTimer(handle,1,20,nullptr);
        if (offline) prepare_offline(); else navigate();
    }
    // 按模式导航到正式主面板或固定探针文档。
    // 步骤：选择受信入口 → 为探针附加固定场景片段 → 提交导航。
    void navigate() {
        if (main_panel) { require(view->Navigate(dfy::ui::main_panel_url.data())); return; }
        const wchar_t* fragment = offline ? L"#offline" : smoke ? L"#verify" : L"";
        if (room_preview) fragment = room_busy ? L"#room-busy" : L"#room-idle";
#ifdef DFY_UI_RUN_STATUS_TEST
        if (run_status_probe) fragment = L"#run-status";
#endif
        require(view->Navigate((std::wstring(page)+fragment).c_str()));
    }
    // 导航前设置当前 WebView2 的离线模拟，不改变系统网络或其他浏览器。
    // 步骤：保留准备责任 → 提交固定 CDP 命令 → 核对结果 → 按关闭状态导航或回收。
    void prepare_offline() {
        ++pending; const auto weak = weak_from_this();
        const auto status = view->CallDevToolsProtocolMethod(L"Network.emulateNetworkConditions",
            L"{\"offline\":true,\"latency\":0,\"downloadThroughput\":-1,\"uploadThroughput\":-1}",
            Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            // 只有协议明确成功才启动页面，失败不退回在线模式冒充通过。
            // 步骤：归还回调责任 → 核对协议回执 → 导航或继续关闭。
            [weak](HRESULT result,LPCWSTR raw) -> HRESULT {
                if (const auto self = weak.lock()) {
                    --self->pending;
                    try {
                        require(result);
                        if (!raw || nlohmann::json::parse(dfy::ui::utf8(raw)).contains("error")) throw std::runtime_error("U01: offline emulation rejected");
                        self->offline_enabled = true;
                        if (!self->closing) self->navigate();
                    } catch (const std::exception& error) { self->fail(error.what()); }
                    catch (...) { self->fail(); }
                    if (self->closing) self->close();
                }
                return S_OK;
            }).Get());
        if (FAILED(status)) { --pending; require(status); }
    }
    // 按关闭或验证状态推进消息，不以导航成功替代授权和退出验收。
    // 步骤：分流收尾状态 → 启动或推进协议 → 核对完成条件与期限。
    void tick() noexcept {
        if (closing) { finish_close(); return; }
        if (run_status_host && run_status_host->exited()) { fail("UI application host stopped"); return; }
        if (!smoke && !interaction_mode) return;
        if (loaded && interaction_mode && !interaction) {
            try {
                const auto weak = weak_from_this();
                if (!lifecycle_mode && !run_status_host) {
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
#ifdef DFY_UI_RUN_STATUS_TEST
                if (room_preview) { interaction->start_room_preview(room_busy); return; }
                if (main_panel_check) { interaction->start_main_panel(checks,panel_gate); return; }
                if (run_status_probe) {
                    const auto probe = run_status_probe;
                    interaction->start_run_status([probe] { probe->submit(); },[probe] { probe->approve(); });
                    return;
                }
#endif
                if (lifecycle_mode) interaction->start_lifecycle(checks);
                else interaction->start();
            } catch (const std::exception& error) { fail(error.what()); }
            catch (...) { fail(); }
        }
        if (interaction) interaction->advance();
        if (smoke && loaded && (offline ? offline_enabled : content->blocked_requests() != 0) && checks->verified()) PostMessageW(handle,WM_CLOSE,0,0);
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
        if (close_controller) PostMessageW(handle,WM_CLOSE,0,0);
    }
    // 在业务线程创建拥有运行状态的 Application，测试使用独占路径和显式替身。
    // 步骤：选择固定测试入口或普通配置 → 移交业务线程 → 保留窗口消息的接收端。
    void start_application() {
#ifdef DFY_UI_RUN_STATUS_TEST
        if (run_status_probe) {
            const auto probe = run_status_probe;
            const auto work = profile.path()/L"agent";
            run_status_host = std::make_unique<dfy::ui::RunStatusHost>([probe,work] { return probe->open(work); },
                [probe](auto& application) { probe->drive(application); });
            return;
        }
        if (main_panel_check) {
            const auto work = profile.path()/L"agent";
            panel_gate = std::make_shared<dfy::tests::UiDialogueGate>();
            run_status_host = std::make_unique<dfy::ui::RunStatusHost>([work,gate = panel_gate] {
                return dfy::tests::open_panel_fixture(work,gate);
            });
            return;
        }
#endif
        if (business_mode) {
            const auto root = assets.parent_path();
            const auto config = model_config.empty() ? root/"config"/"deepseek.example.json" : model_config;
            run_status_host = std::make_unique<dfy::ui::RunStatusHost>([root,config] {
                return std::make_unique<dfy::app::Application>(dfy::model::load_config(config),
                    root/"plugins",dfy::app::default_user_data());
            });
        }
    }
    // 创建独立环境，配置路径与回调寿命由当前窗口拥有。
    // 步骤：检查离线入口 → 提交环境创建 → 接收环境或错误 → 按窗口状态继续或收尾。
    void start() {
        if (!std::filesystem::is_regular_file(assets/(main_panel ? L"main-panel.html" : L"index.html")))
            throw std::runtime_error("UI assets missing");
        profile.create();
        start_application();
        const auto weak = weak_from_this(); ++pending;
        const auto status = CreateCoreWebView2EnvironmentWithOptions(nullptr,profile.path().c_str(),nullptr,
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
        if (early) PostMessageW(handle,WM_CLOSE,0,0);
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
        if (message == WM_APP+1) { KillTimer(window,2); DestroyWindow(window); return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(state->result); return 0; }
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
// 在 STA 中运行桌面容器，普通入口启用独立业务宿主。
// 步骤：核对参数与运行库 → 创建窗口及环境 → 推进消息循环 → 释放 COM 公寓。
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    const bool offline = __argc == 2 && std::wstring_view(__wargv[1]) == L"--offline";
    const bool smoke = offline || (__argc == 2 && std::wstring_view(__wargv[1]) == L"--smoke");
    const bool interaction_mode = __argc == 2 && std::wstring_view(__wargv[1]) == L"--interaction";
    const bool lifecycle_mode = __argc == 2 && std::wstring_view(__wargv[1]) == L"--lifecycle";
    const bool early = __argc == 2 && std::wstring_view(__wargv[1]) == L"--close-early";
    const bool close_controller = __argc == 2 && std::wstring_view(__wargv[1]) == L"--close-controller";
    const bool missing_runtime = __argc == 2 && std::wstring_view(__wargv[1]) == L"--missing-runtime";
    const bool sample = __argc == 3 && (std::wstring_view(__wargv[2]) == L"idle" || std::wstring_view(__wargv[2]) == L"busy");
    const bool room_preview = sample && std::wstring_view(__wargv[1]) == L"--room-preview";
    const bool main_panel = (__argc == 2 || __argc == 3) && std::wstring_view(__wargv[1]) == L"--main-panel";
#ifdef DFY_UI_RUN_STATUS_TEST
    const bool room_check = sample && std::wstring_view(__wargv[1]) == L"--check-room-preview";
    const bool run_status_mode = __argc == 3 && std::wstring_view(__wargv[1]) == L"--run-status";
    const bool main_panel_check = __argc == 2 && std::wstring_view(__wargv[1]) == L"--check-main-panel";
#else
    const bool room_check = false;
    const bool run_status_mode = false;
    const bool main_panel_check = false;
#endif
    const bool automatic = smoke || interaction_mode || lifecycle_mode || early || close_controller || missing_runtime || run_status_mode || room_check || main_panel_check;
    if (__argc > 1 && !automatic && !room_preview && !main_panel) return 2;
    if (FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 3;
    int result = 1;
    try {
        const auto missing = dfy::app::executable_directory()/L"__u01_missing_runtime__";
        if (missing_runtime && std::filesystem::exists(missing)) throw std::runtime_error("U01: missing-runtime fixture must not exist");
        LPWSTR raw = nullptr; const auto status = GetAvailableCoreWebView2BrowserVersionString(missing_runtime ? missing.c_str() : nullptr,&raw);
        std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> version(raw,&CoTaskMemFree);
        if (FAILED(status) || !version) {
            std::fputs("WebView2 Runtime unavailable\n",stderr);
            if (!automatic) MessageBoxW(nullptr,L"请安装 Microsoft Edge WebView2 Evergreen Runtime。",L"大肥鱼管理中心",MB_OK|MB_ICONERROR);
            CoUninitialize(); return 4;
        }
        auto state = std::make_shared<Window>(); state->smoke = smoke; state->interaction_mode = interaction_mode || lifecycle_mode || run_status_mode; state->lifecycle_mode = lifecycle_mode; state->early = early;
        state->close_controller = close_controller;
        state->offline = offline;
        state->room_preview = room_preview || room_check;
        state->room_busy = state->room_preview && std::wstring_view(__wargv[2]) == L"busy";
        state->main_panel = main_panel || main_panel_check;
        state->main_panel_check = main_panel_check;
        if (main_panel && __argc == 3) state->model_config = __wargv[2];
        state->interaction_mode |= room_check || main_panel_check;
        state->business_mode = !automatic && !room_preview;
#ifdef DFY_UI_RUN_STATUS_TEST
        if (run_status_mode) state->run_status_probe = std::make_shared<dfy::tests::UiRunStatusProbe>(__wargv[2]);
#endif
        state->assets = dfy::app::executable_directory()/L"ui";
        WNDCLASSW kind{}; kind.lpfnWndProc = procedure; kind.hInstance = instance; kind.lpszClassName = L"DaFeiYuU01";
        kind.hCursor = LoadCursorW(nullptr,IDC_ARROW); if (!RegisterClassW(&kind)) throw std::runtime_error("window class failed");
        const DWORD extended = automatic ? WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW : 0;
        const auto title = state->main_panel ? L"大肥鱼" : state->room_preview ? L"大肥鱼管理中心 · 首屏预览" : L"大肥鱼管理中心 · U01 验证";
        state->handle = CreateWindowExW(extended,kind.lpszClassName,title,WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,CW_USEDEFAULT,1100,760,nullptr,nullptr,instance,state.get());
        if (!state->handle) throw std::runtime_error("window creation failed");
        if (state->interaction_mode) {
            // 保留浏览器的绘制与输入路由，同时由桌面合成器隐藏验收窗口。
            const BOOL cloaked = TRUE;
            require(DwmSetWindowAttribute(state->handle,DWMWA_CLOAK,&cloaked,sizeof(cloaked)));
            ShowWindow(state->handle,SW_SHOWNOACTIVATE);
            DWORD flags = 0;
            require(DwmGetWindowAttribute(state->handle,DWMWA_CLOAKED,&flags,sizeof(flags)));
            if (!(flags & DWM_CLOAKED_APP) || GetForegroundWindow() == state->handle)
                throw std::runtime_error("UI: interaction fixture must remain hidden and inactive");
        } else ShowWindow(state->handle,automatic ? SW_HIDE : show);
        try { state->start(); } catch (const std::exception& error) { state->fail(error.what()); }
        catch (...) { state->fail(); }
        MSG message{}; BOOL next;
        while ((next = GetMessageW(&message,nullptr,0,0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        result = next < 0 ? 1 : static_cast<int>(message.wParam);
        const nlohmann::json report{{"result",result},{"pending_callbacks",state->pending},{"pending_at_close",state->pending_at_close},
            {"protocol_pending",state->interaction && state->interaction->pending()},
            {"browser_processes",state->processes.count()},{"processes_exited",!state->process_check_failed && state->processes.exited()},
            {"profile_cleaned",state->profile.cleaned()},{"active_frames",state->checks ? state->checks->active : 0},
            {"live_channels",state->checks ? state->checks->live_channels : 0},
            {"created_frames",state->checks ? state->checks->created : 0},{"destroyed_frames",state->checks ? state->checks->destroyed : 0},
            {"runtime",dfy::ui::utf8(version.get())},{"root_navigation_ms",state->navigation_ms},
            {"offline_enabled",state->offline_enabled},
            {"elapsed_ms",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-state->started).count()}};
        std::puts(report.dump().c_str());
    } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); result = 1; }
    catch (...) { result = 1; }
    CoUninitialize(); return result;
}
