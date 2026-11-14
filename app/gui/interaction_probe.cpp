#include "interaction_probe.hpp"
#include <cstdio>

namespace dfy::ui {
// 固定浏览器与最终结果接收方，不在构造阶段运行协议。
InteractionProbe::InteractionProbe(ICoreWebView2* view, std::function<void(bool)> finished) : view_(view), finished_(std::move(finished)) {}
// 保存一项固定方法与参数，后续由唯一推进流程执行。
void InteractionProbe::call(std::string name, std::wstring method, Json parameters, std::function<void(const Json&)> check) {
    steps_.push_back({std::move(name),std::move(method),std::move(parameters),false,std::move(check)});
}
// 断言结果必须明确为布尔真，JavaScript 异常或其他值均拒绝。
// 步骤：准备固定求值参数 → 登记断言 → 固定主文档或 Frame 上下文。
void InteractionProbe::evaluate(std::string name, std::string script, bool frame) {
    call(std::move(name),L"Runtime.evaluate",{{"expression",std::move(script)},{"returnByValue",true},{"awaitPromise",true}},[](const Json& reply) {
        if (reply.contains("exceptionDetails") || !reply.at("result").value("value",false)) throw std::runtime_error("DOM assertion failed");
    });
    steps_.back().frame = frame;
}
// 只轮询无副作用条件，超时仍明确失败；不反复提交输入或点击。
void InteractionProbe::expect(std::string name, std::string condition, bool frame) {
    evaluate(std::move(name),"(async()=>{ const end=performance.now()+3000; while(performance.now()<end){ if("+condition+") return true; await new Promise(r=>setTimeout(r,20)); } return false; })()",frame);
}
// 串行提交按下和释放，避免浏览器留下修饰键状态。
// 步骤：准备虚拟键及修饰符 → 按 Enter 补充字符事件 → 提交按下和释放。
void InteractionProbe::key(std::string name, std::string value, int code, int modifiers) {
    for (const auto type : {"rawKeyDown","keyUp"}) {
        Json parameters{{"type",type},{"key",value},{"code",value},{"windowsVirtualKeyCode",code},{"nativeVirtualKeyCode",code},{"modifiers",modifiers}};
        if (code == 13 && std::string_view(type) == "rawKeyDown") { parameters["type"] = "keyDown"; parameters["text"] = "\r"; parameters["unmodifiedText"] = "\r"; }
        call(name+"/"+type,L"Input.dispatchKeyEvent",std::move(parameters));
    }
}
// 在真实 WebView2 内验证输入组合和焦点，所有检查由固定协议序列产生。
// 步骤：模拟页面聚焦 → 主页面组合提交 → 对话框焦点与 Escape → 独立 Frame 输入和原生回显。
void InteractionProbe::start() {
    call("focus emulation",L"Emulation.setFocusEmulationEnabled",{{"enabled",true}});
    evaluate("main focus","document.querySelector('#main-text').focus(); document.activeElement.id === 'main-text'");
    evaluate("track input event kinds","window.__u01_events=[]; for(const type of ['compositionstart','compositionend','input','change']) document.querySelector('#main-text').addEventListener(type,e=>window.__u01_events.push({type:e.type,trusted:e.isTrusted,composing:e.isComposing ?? null})); true");
    call("main composition",L"Input.imeSetComposition",{{"text","大肥"},{"selectionStart",2},{"selectionEnd",2}});
    evaluate("composition in progress","document.querySelector('#main-text').value === '大肥' && document.body.textContent.includes('正在选字')");
    call("main commit",L"Input.insertText",{{"text","大肥鱼中文验证"}});
    call("main committed value",L"Runtime.evaluate",{{"expression","({input:document.querySelector('#main-text').value === '大肥鱼中文验证', echo:document.querySelector('#main-echo').textContent === '大肥鱼中文验证', count:Number(document.querySelector('#composition-count').textContent), composing:document.body.textContent.includes('正在选字'), events:window.__u01_events})"},{"returnByValue",true}},[](const Json& reply) {
        const auto& state = reply.at("result").at("value");
        if (!state.at("input").get<bool>() || !state.at("echo").get<bool>() || state.at("count").get<int>() < 1 || state.at("composing").get<bool>())
            throw std::runtime_error("input state: "+state.dump());
    });
    key("Tab to next","Tab",9);
    expect("next input focus","document.activeElement.id === 'next-text'");
    key("Shift Tab","Tab",9,8);
    expect("return input focus","document.activeElement.id === 'main-text'");
    evaluate("dialog trigger focus","document.querySelector('#open-panel').focus(); document.activeElement.id === 'open-panel'");
    key("open dialog with Enter","Enter",13);
    expect("dialog initial focus","document.activeElement.id === 'panel-text'");
    key("dialog Tab","Tab",9);
    expect("dialog next focus","document.activeElement.id === 'panel-next'");
    key("dialog close focus","Tab",9);
    expect("dialog close button","document.activeElement.id === 'close-panel'");
    key("dialog focus wrap","Tab",9);
    expect("dialog trap","document.activeElement.id === 'panel-text'");
    call("dialog composition",L"Input.imeSetComposition",{{"text","候选"},{"selectionStart",2},{"selectionEnd",2}});
    key("Escape during composition","Escape",27);
    evaluate("dialog remains during composition","(async()=>{ await new Promise(r=>setTimeout(r,100)); return document.querySelector('[role=dialog]') !== null; })()");
    call("cancel composition",L"Input.imeSetComposition",{{"text",""},{"selectionStart",0},{"selectionEnd",0}});
    key("Escape closes dialog","Escape",27);
    expect("dialog close settled","!document.querySelector('[role=dialog]') && document.activeElement.id === 'open-panel'");
    call("dialog closed and focus returned",L"Runtime.evaluate",{{"expression","(async()=>{ await new Promise(r=>setTimeout(r,50)); return {dialog:!!document.querySelector('[role=dialog]'),active:document.activeElement.id,visibility:document.visibilityState,composing:document.body.textContent.includes('正在选字')}; })()"},{"awaitPromise",true},{"returnByValue",true}},[](const Json& reply) {
        const auto& state = reply.at("result").at("value");
        if (state.at("dialog").get<bool>() || state.at("active") != "open-panel") throw std::runtime_error("dialog state: "+state.dump());
    });
    call("get frame",L"Page.getFrameTree",Json::object(),[this](const Json& reply) {
        frame_id_ = reply.at("frameTree").at("childFrames").at(0).at("frame").at("id").get<std::string>();
    });
    call("frame context",L"Page.createIsolatedWorld",{{"worldName","dfy-u01-check"},{"grantUniveralAccess",false}},[this](const Json& reply) {
        context_ = reply.at("executionContextId").get<int>();
    });
    evaluate("frame focus","document.querySelector('#frame-text').focus(); document.activeElement.id === 'frame-text'",true);
    call("frame composition",L"Input.imeSetComposition",{{"text","独立"},{"selectionStart",2},{"selectionEnd",2}});
    call("frame commit",L"Input.insertText",{{"text","独立页面中文"}});
    evaluate("frame committed","document.querySelector('#frame-text').value === '独立页面中文' && Number(document.querySelector('#composition').dataset.count) > 0",true);
    evaluate("frame echo","document.querySelector('#echo').click(); true",true);
    evaluate("native echo received","(async()=>{ const end=performance.now()+3000; while(performance.now()<end){ if(document.querySelector('#result').textContent === 'allowed：独立页面中文') return true; await new Promise(r=>setTimeout(r,20)); } return false; })()",true);
    next();
}
// 每项协议只在上一项回调完成后执行，晚到回调不能重启已经关闭的验收。
// 步骤：完成或提交当前项 → 处理协议错误 → 核对结果 → 递交下一项或报告失败。
void InteractionProbe::next() noexcept {
    if (closed_) return;
    if (current_ == steps_.size()) { finished_(true); return; }
    try {
        const auto& step = steps_[current_]; auto parameters = step.parameters;
        if (step.frame) parameters["contextId"] = context_;
        if (step.method == L"Page.createIsolatedWorld") parameters["frameId"] = frame_id_;
        const auto weak = weak_from_this();
        require_hresult(view_->CallDevToolsProtocolMethod(step.method.c_str(),utf16(parameters.dump()).c_str(),
            Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            // 复制协议返回值后再验证，失败时只输出固定检查名，不记录页面正文。
            // 步骤：过滤失效宿主 → 检查返回及断言 → 推进计数或报告失败。
            [weak](HRESULT status,LPCWSTR raw) -> HRESULT {
                if (const auto self = weak.lock(); self && !self->closed_) {
                    try {
                        require_hresult(status);
                        if (!raw) throw std::runtime_error("Missing CDP response");
                        const auto reply = Json::parse(utf8(raw));
                        if (reply.contains("error")) throw std::runtime_error("CDP command rejected");
                        if (self->steps_[self->current_].check) self->steps_[self->current_].check(reply);
                        ++self->current_; self->next();
                    } catch (const std::exception& error) {
                        std::fprintf(stderr,"Interaction check failed: %s (%s)\n",self->steps_[self->current_].name.c_str(),error.what());
                        self->finished_(false);
                    } catch (...) { self->finished_(false); }
                }
                return S_OK;
            }).Get()));
    } catch (...) { finished_(false); }
}
}
