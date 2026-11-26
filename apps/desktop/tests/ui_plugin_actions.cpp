#include "../src/interaction_probe.hpp"
#include <fstream>

namespace dfy::ui {
// 在同一本体上用真实点击管理测试插件，跨文档和独立客户端按原号复查。
// 步骤：保护两窗各自的选中与旧目录 → 实际加载并刷新补读 → 实际卸载重载 → 新窗口核对原回执。
void InteractionProbe::start_plugin_actions(std::wstring_view mode, const std::filesystem::path& data) {
    // 等待同伴通过真正的窗口完成指定检查。
    const auto wait = [&](const char* name) { wait_for(name,[file=data/name] { return std::filesystem::is_regular_file(file); }); };
    // 保存原窗口实际收到的公开回执，供其他已握手窗口复核。
    const auto save = [&](const char* name, const char* expression) {
        call(name,L"Runtime.evaluate",{{"expression",expression},{"returnByValue",true}},[file=data/name](const Json& reply) {
            std::ofstream(file) << reply.at("result").at("value").dump();
        });
    };
    // 每个真实文档单独观察消息，固定故障只暂缓指定读口，写入均进入正式 IPC。
    // 步骤：挂接观察 → 统计真实提交 → 打开本窗设置。
    const auto open = [&] {
        expect("real resident handshake","document.querySelector('#dialogue-state')?.dataset.ready==='true'");
        evaluate("open formal package settings and observe public messages",R"JS((()=>{
            const bridge=window.chrome.webview, post=bridge.postMessage.bind(bridge);
            window.lastPacket=new Map(); window.holdReads=''; window.held=[]; window.opReplies=[];
            window.button=id=>document.getElementById(id);
            window.choice=()=>document.querySelector('[data-selected-package]');
            window.work=()=>document.querySelector('.plugin-operation');
            window.row=()=>window.currentDirectory?.packages.find(p=>p.id==='test.second');
            Object.defineProperty(bridge,'postMessage',{configurable:true,value:q=>{
                if(q.type===window.holdReads){window.held.push(q);return;}
                if(q.type==='plugins.load'||q.type==='plugins.unload')sessionStorage.setItem('observed-writes',String(Number(sessionStorage.getItem('observed-writes'))+1));
                post(q);
            }});
            bridge.addEventListener('message',({data:q})=>{
                lastPacket.set(q.request,q);
                if(q.event==='plugins.directory' && q.state==='ready')window.currentDirectory=q;
                if(q.event==='plugins.operation')opReplies.push(q);
            });
            button('board-settings').click(); return true;
        })())JS");
        expect("verified package directory","typeof row==='function' && !!row() && document.querySelector('.plugin-settings').dataset.directoryStatus==='ready'");
    };
    // 用本窗选定包的可见按钮精确提交，不从测试构造管理写口。
    const auto select = [&] {
        evaluate("select installed fixture","document.querySelector('[data-package=\"test.second\"]').click(); true");
        expect("original package selected","choice()?.dataset.selectedPackage==='test.second'");
    };
    open();
    if (mode==L"action-reopen") {
        Json old_load, old_unload;
        std::ifstream(data/"first-load.json") >> old_load;
        std::ifstream(data/"original-unload.json") >> old_unload;
        evaluate("recover original operation IDs in a new client","window.oldLoad="+old_load.dump()+";window.oldUnload="+old_unload.dump()+R"JS(;
            for(const [request,receipt] of [['check-load',oldLoad],['check-unload',oldUnload]])chrome.webview.postMessage({
                type:'plugins.receipt',version:'1',request,instance:receipt.instance,operation:receipt.operation});
            chrome.webview.postMessage({type:'plugins.receipt',version:'1',request:'wrong-resident',instance:'other-host',operation:oldLoad.operation}); true)JS");
        expect("known operations are readable only on their original resident","lastPacket.get('check-load')?.effect==='loaded' && lastPacket.get('check-load').operation===oldLoad.operation && "
            "lastPacket.get('check-load').generation===oldLoad.generation && lastPacket.get('check-unload')?.effect==='unloaded' && "
            "lastPacket.get('check-unload').targetGeneration===oldLoad.generation && lastPacket.get('wrong-resident')?.state==='stale' && "
            "row().loaded && row().generation!==oldLoad.generation && !work() && !sessionStorage.getItem('observed-writes')");
        next(); return;
    }
    select();
    if (mode==L"action-observe") {
        wait("writer-ready");
        evaluate("pin a real earlier sample before the peer acts","window.holdReads='plugins.list'; row().generation==='0' && !button('plugin-load').disabled");
        save("observer-pinned","currentDirectory.revision");
        wait("first-load.json");
        evaluate("explicit click on the earlier revision","button('plugin-load').click(); true");
        expect("resident rejected obsolete revision without changing the plugin","work()?.dataset.workState==='stale' && work().dataset.workEffect==='notApplied' && "
            "opReplies.some(q=>q.effect==='notApplied' && q.revision===currentDirectory.revision && q.targetGeneration==='0')");
        evaluate("resume this window's directory","window.holdReads=''; window.held.splice(0).forEach(q=>chrome.webview.postMessage(q)); button('refresh-plugins').click(); true");
        expect("loaded instance is visible after stale submission","row()?.loaded && choice()?.dataset.selectedGeneration===row().generation");
        save("observer-loaded","row().generation");
        wait("original-unload.json");
        evaluate("read peer unload","button('refresh-plugins').click(); true");
        expect("actual unload reaches the observing window","!row()?.loaded && choice()?.dataset.selectedGeneration==='0'");
        save("observer-unloaded","row().generation");
        next(); return;
    }
    evaluate("interleave a harmless independent dialogue command",R"JS((()=>{
        chrome.webview.postMessage({type:'dialogue.cancel',version:'1',request:'fixture-chat-command',command:'1',session:'999',turn:'999'});
        return true;
    })())JS");
    expect("dialogue command number one was answered","lastPacket.get('fixture-chat-command')?.receipt?.command==='1'");
    save("writer-ready","currentDirectory.instance");
    wait("observer-pinned");
    evaluate("one double click on an installed fixture","window.holdReads='plugins.receipt'; const b=button('plugin-load'); b.click(); b.click(); sessionStorage.getItem('observed-writes')==='1'");
    expect("plugin command one has its own actual acceptance","work()?.dataset.workState==='queued' && opReplies.some(q=>q.state==='queued' && q.command==='1' && q.id==='test.second' && q.targetGeneration==='0')");
    evaluate("remember original host-issued identity","sessionStorage.setItem('original-operation',opReplies.find(q=>q.state==='queued').operation); true");
    call("reload after real admission",L"Page.reload",Json::object());
    expect("new main document loaded","performance.getEntriesByType('navigation')[0]?.type==='reload' && !window.lastPacket && !!document.querySelector('#board-settings')");
    open();
    expect("reloaded document only queried the actual load","work()?.dataset.workEffect==='loaded' && row()?.loaded && !choice() && sessionStorage.getItem('observed-writes')==='1' && "
        "opReplies.some(q=>q.operation===sessionStorage.getItem('original-operation') && q.generation===row().generation && q.effect==='loaded')");
    save("first-load.json","opReplies.find(q=>q.effect==='loaded')");
    wait("observer-loaded");
    select();
    evaluate("unload only the actual selected generation","window.firstGeneration=row().generation; button('plugin-unload').click(); true");
    expect("original instance has actually exited","work()?.dataset.workEffect==='unloaded' && !row()?.loaded && "
        "opReplies.some(q=>q.effect==='unloaded' && q.command==='2' && q.targetGeneration===firstGeneration)");
    save("original-unload.json","opReplies.find(q=>q.effect==='unloaded')");
    wait("observer-unloaded");
    evaluate("explicitly reload after actual exit","button('plugin-load').click(); true");
    expect("reload creates a different actual generation","work()?.dataset.workEffect==='loaded' && row()?.loaded && row().generation!==firstGeneration && "
        "opReplies.some(q=>q.command==='3' && q.generation===row().generation && q.effect==='loaded') && sessionStorage.getItem('observed-writes')==='3'");
    next();
}
}
