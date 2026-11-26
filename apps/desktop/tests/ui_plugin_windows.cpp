#include "../src/interaction_probe.hpp"
#include <fstream>

namespace dfy::ui {
// 先从无声明实例读取，重载后以旧代次复查，再核实新代次的默认值和用户覆盖。
// 步骤：选择无声明插件 → 保留原查询等待重载 → 核对过期回执 → 仅呈现已发布的字段。
void InteractionProbe::start_plugin_fields(const std::filesystem::path& data) {
    expect("original public field session","document.querySelector('#dialogue-state')?.dataset.ready==='true'");
    evaluate("observe original-bound public settings",R"JS((()=>{
        const b=chrome.webview, post=b.postMessage.bind(b);
        window.holdField=false;window.holdList=false;window.delayedFields=[];window.delayedList=[];window.publicReplies=[];window.fieldWrites=0;
        window.fields=()=>document.querySelector('.public-fields');
        Object.defineProperty(b,'postMessage',{configurable:true,value:q=>{
            if(q.type==='settings.save' && q.owner==='plugin')++window.fieldWrites;
            if(q.type==='plugins.list' && holdList)delayedList.push(q);
            else if(q.type==='settings.read' && q.owner==='plugin' && holdField)delayedFields.push(q); else post(q);
        }});
        b.addEventListener('message',({data:q})=>{if(q.event==='settings.snapshot' && q.owner==='plugin')publicReplies.push(q);});
        document.querySelector('#board-settings').click();return true;
    })())JS");
    expect("both real packages listed","!!document.querySelector('[data-package=\"test.second\"]') && !fields()");
    evaluate("select the undeclared original instance","document.querySelector('[data-package=\"test.mail\"]').click();true");
    expect("explicit nondeclaration from original owner","fields()?.dataset.fieldsState==='notDeclared' && !fields().querySelector('dd') && publicReplies.at(-1)?.state==='notDeclared'");
    evaluate("switch to an installed-only package","window.oldGeneration=fields().dataset.fieldsGeneration;window.holdList=true;document.querySelector('[data-package=\"test.second\"]').click();true");
    expect("installed-only selection cannot inherit original settings","!!document.querySelector('[data-selected-package=\"test.second\"]') && !fields()");
    evaluate("request original generation again","window.holdField=true;document.querySelector('[data-package=\"test.mail\"]').click();true");
    expect("original request is pinned before reload","delayedFields.length===1 && delayedFields[0].generation===oldGeneration && fields()?.dataset.fieldsState==='loading'");
    wait_for("public-before",[file=data/"public-before"] { std::ofstream(file).put('1'); return true; });
    wait_for("public-reloaded",[file=data/"public-reloaded"] { return std::filesystem::is_regular_file(file); });
    evaluate("release the old field query on the real pipe","window.holdField=false;delayedFields.splice(0).forEach(q=>chrome.webview.postMessage(q));true");
    expect("former generation yields a visible stale state without fields","fields()?.dataset.fieldsState==='stale' && !fields().querySelector('dd') && publicReplies.at(-1)?.generation===oldGeneration");
    evaluate("refresh directory to the actual running generation","window.holdList=false;delayedList.splice(0).forEach(q=>chrome.webview.postMessage(q));document.querySelector('#refresh-plugins').click();true");
    expect("only registered applied strings are visible on the new instance","fields()?.dataset.fieldsState==='ready' && fields().dataset.fieldsGeneration!==oldGeneration && "
        "fields().querySelectorAll('[data-public-field]').length===2 && fields().querySelector('[data-public-field=label]').textContent.includes('user label') && "
        "fields().querySelector('[data-public-field=label]').textContent.includes('用户设置') && fields().querySelector('[data-public-field=mode] span').textContent==='t' && "
        "fields().querySelector('[data-public-field=mode]').textContent.includes('包默认值') && !fields().querySelector('input,textarea,button') && fieldWrites===0 && "
        "!document.body.textContent.includes('private-fixture-sentinel') && !JSON.stringify(publicReplies).includes('private-fixture-sentinel')");
    next();
}

// 窗口独立选择实际包，由夹具控制端在两窗共同采样后重载原插件。
// 步骤：读取原目录并选包 → 保存本窗读数 → 核对同一后台的新修订及新代次。
void InteractionProbe::start_plugin_windows(std::wstring_view mode, const std::filesystem::path& data) {
    const bool manager = mode==L"plugins-manage";
    expect("original resident handshake","document.querySelector('#dialogue-state')?.dataset.ready==='true'");
    evaluate("open the native plugin directory",R"JS((()=>{
        window.pane=()=>document.querySelector('.plugin-settings');
        window.chosen=()=>document.querySelector('[data-selected-package]');
        window.chrome.webview.addEventListener('message',({data})=>{
            if(data.event==='plugins.directory' && data.state==='ready') window.observedPackages=data;
        });
        document.querySelector('#board-settings').click(); return true;
    })())JS");
    expect("both packages come from the real resident","pane()?.dataset.directoryStatus==='ready' && "
        "document.querySelector('[data-package=\"test.mail\"]')?.textContent.includes('已加载') && "
        "document.querySelector('[data-package=\"test.second\"]')?.textContent.includes('已安装') && !chosen()");
    const auto target = manager ? "test.mail" : "test.second";
    evaluate("explicit selection in this window","window.packageChoice="+Json(target).dump()+
        "; document.querySelector('[data-package=\"'+packageChoice+'\"]').click(); true");
    expect("selection comes from this window","chosen()?.dataset.selectedPackage===packageChoice");
    evaluate("remember original resident, revision and active generation",R"JS((()=>{
        const sample=window.observedPackages;
        window.originalDirectory=[pane().dataset.directoryInstance,pane().dataset.directoryRevision,sample.packages.find(p=>p.id==='test.mail').generation];
        return originalDirectory[0]===sample.instance && originalDirectory[1]===sample.revision && originalDirectory[2]!=='0';
    })())JS");
    call("write each original sample",L"Runtime.evaluate",{{"expression","originalDirectory"},{"returnByValue",true}},
        [file=data/(manager ? "plugins-manage.json" : "plugins-watch.json")](const Json& reply) {
            std::ofstream(file) << reply.at("result").at("value").dump();
        });
    wait_for("control process reloaded the original generation",[file=data/"plugin-reloaded"] { return std::filesystem::is_regular_file(file); });
    evaluate("request a fresh original directory","document.querySelector('#refresh-plugins').click(); true");
    expect("both windows read the new generation on their original resident","pane()?.dataset.directoryStatus==='ready' && "
        "pane().dataset.directoryInstance===originalDirectory[0] && pane().dataset.directoryRevision!==originalDirectory[1] && "
        "observedPackages.packages.some(p=>p.id==='test.mail' && p.loaded && p.generation!=='0' && p.generation!==originalDirectory[2]) && "
        "chosen()?.dataset.selectedPackage===packageChoice && chosen().dataset.selectedGeneration===observedPackages.packages.find(p=>p.id===packageChoice).generation");
    next();
}
}
