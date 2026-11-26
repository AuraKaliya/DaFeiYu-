#include "../src/interaction_probe.hpp"

namespace dfy::ui {
// 仅替换插件目录读口的回包，在正式设置页面覆盖可用、过期和重连的反馈。
// 步骤：选中原实例的包 → 核对失败时的旧列表 → 更换后台并读取空目录。
void InteractionProbe::start_plugin_settings() {
    const Json rows=Json::array({{{"id","test.one"},{"name","一号插件"},{"version","1.2"},{"state","installed"},
        {"installed",true},{"loaded",false},{"generation","0"},{"canLoad",true},{"canUnload",false}},
        {{"id","test.two"},{"name","二号插件"},{"version","2"},{"state","active"},{"generation","97"},
        {"installed",true},{"loaded",true},{"canLoad",false},{"canUnload",true},{"loadedName","旧实例"},{"loadedVersion","1.7"},{"errorCode",0}}});
    // 从页面实际发出的目录请求取身份，由原生 WebView2 消息口投递固定信封。
    // 步骤：等候当前请求 → 核对操作类型 → 回显原号并交还固定结果。
    const auto reply = [&](Json body) {
        expect("sample directory request arrived","window.fixturePluginReads.length>0");
        call("return the original plugin directory request",L"Runtime.evaluate",
            {{"expression","window.fixturePluginReads.shift()"},{"returnByValue",true}},
            [this,body=std::move(body)](const Json& result) mutable {
                const auto& request=result.at("result").at("value");
                if (request.at("type")!="plugins.list" || request.at("version")!="1") throw std::runtime_error("unexpected plugin operation");
                body.update(Json{{"version","1"},{"request",request.at("request")}});
                require_hresult(view_->PostWebMessageAsJson(utf16(body.dump()).c_str()));
            });
    };
    // 用本窗的刷新按钮发起下一次独立查询。
    const auto refresh = [&] { evaluate("refresh visible packages","document.querySelector('#refresh-plugins').click(); true"); };
    expect("offline model fixture is ready","document.querySelector('#dialogue-state')?.dataset.ready==='true'");
    call("focus emulation",L"Emulation.setFocusEmulationEnabled",{{"enabled",true}});
    evaluate("visit the settings directory through its formal entry",R"JS((()=>{
        const bridge=window.chrome.webview, post=bridge.postMessage.bind(bridge);
        window.fixturePluginReads=[];
        Object.defineProperty(bridge,'postMessage',{configurable:true,value:fields=>{
            if(fields.type?.startsWith('plugins.')) window.fixturePluginReads.push(fields);
            else post(fields);
        }});
        document.querySelector('#board-settings').click(); return true;
    })())JS");
    Json sample{{"event","plugins.directory"},{"instance","host-a"},{"revision","1234567890123456789"},{"state","ready"},{"packages",rows},{"unavailableCount",0}};
    reply(sample);
    expect("both installed states are visible","document.querySelectorAll('.plugin-settings input[type=radio]').length===2 && "
        "document.querySelector('[data-package=\"test.one\"]').textContent.includes('1.2') && "
        "document.querySelector('[data-package=\"test.two\"]').textContent.includes('已加载')");
    evaluate("focus the first package","const r=document.querySelector('[data-package=\"test.one\"] input'); r.click(); r.focus(); true");
    expect("first radio selection and focus agree","document.querySelector('[data-selected-package=\"test.one\"]') && document.activeElement?.value==='test.one'");
    evaluate("choose through the second visible label","document.querySelector('[data-package=\"test.two\"]').click(); true");
    expect("selection names the actual generation and original running version","document.querySelector('[data-package=\"test.two\"] input').checked && "
        "document.querySelector('[data-selected-package=\"test.two\"]')?.dataset.selectedGeneration==='97' && document.querySelector('.package-details').textContent.includes('旧实例 1.7')");
    call("narrow settings window",L"Emulation.setDeviceMetricsOverride",{{"width",360},{"height",690},{"deviceScaleFactor",1},{"mobile",false}});
    expect("narrow directory fits the settings area","document.documentElement.scrollWidth<=innerWidth && document.querySelector('[data-package]').getBoundingClientRect().right<=innerWidth");
    refresh(); reply({{"event","management.error"},{"reason","private diagnostic"}});
    expect("failed read retains the last packages","document.querySelector('#plugin-directory-status').textContent.includes('暂时无法') && "
        "document.querySelectorAll('.plugin-settings input:disabled').length===2 && !document.body.textContent.includes('private diagnostic')");
    sample.update(Json{{"instance","host-b"},{"revision","1"},{"state","stale"},{"packages",Json::array()}});
    refresh(); reply(sample);
    expect("stale directory is visibly distinct","document.querySelector('#plugin-directory-status').textContent.includes('目录已过期') && !document.querySelector('[data-selected-package]')");
    sample.update(Json{{"state","ready"},{"packages",rows}});
    refresh(); reply(sample);
    expect("new resident does not inherit a selected package","document.querySelector('.plugin-settings').dataset.directoryStatus==='ready' && "
        "document.querySelectorAll('.plugin-settings input[type=radio]').length===2 && !document.querySelector('[data-selected-package]')");
    sample.update(Json{{"revision","2"},{"packages",Json::array()},{"unavailableCount",2}});
    refresh(); reply(sample);
    expect("partial discovery is distinct from empty","document.querySelector('#plugin-directory-status').textContent.includes('2 个目录项无法读取') && !document.querySelector('#plugin-directory-status').textContent.includes('目录为空')");
    sample.update(Json{{"revision","3"},{"unavailableCount",0}});
    refresh(); reply(sample);
    expect("only confirmed empty samples show the empty state","document.querySelector('#plugin-directory-status').textContent.includes('当前插件目录为空') && !document.querySelector('[data-package]')");
    next();
}
}
