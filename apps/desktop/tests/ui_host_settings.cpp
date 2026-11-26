#include "../src/interaction_probe.hpp"
#include <fstream>

namespace dfy::ui {
// 两窗沿各自的原修订保存真实配置，隔离后台重启后核对已采用值和原回执。
// 步骤：保留同伴草稿 → 保存并重开读取 → 验证冲突与明确改绑 → 在重启的新实例查回。
void InteractionProbe::start_host_save(std::wstring_view mode, const std::filesystem::path& data) {
    // 等待另一个窗口已核实指定实效。
    const auto wait = [&](const char* name) { wait_for(name,[file=data/name] { return std::filesystem::is_regular_file(file); }); };
    // 把本窗亲自取得的原回执交给隔离验收流程。
    const auto save = [&](const char* name, const char* expression) {
        call(name,L"Runtime.evaluate",{{"expression",expression},{"returnByValue",true}},[file=data/name](const Json& reply) {
            std::ofstream(file) << reply.at("result").at("value").dump();
        });
    };
    // 为新文档安装只读观察并打开真实本体设置，写入直接进入原生客户端。
    // 步骤：等待握手 → 收集配置事实和原回执 → 展开本窗设置。
    const auto open = [&] {
        expect("host settings handshake","document.querySelector('#dialogue-state')?.dataset.ready==='true'");
        evaluate("observe effective configuration and stable submissions",R"JS((()=>{
            const b=chrome.webview, post=b.postMessage.bind(b);window.holdConfig=false;window.heldConfig=[];
            window.el=id=>document.getElementById(id);window.historyOfSaves=[];window.byRequest=new Map();
            window.editTimeout=v=>{const field=el('host-timeout-input');field.value=v;field.dispatchEvent(new Event('input',{bubbles:true}));};
            Object.defineProperty(b,'postMessage',{configurable:true,value:q=>{
                if(q.type==='settings.read' && window.holdConfig){heldConfig.push(q);return;}
                if(q.type==='settings.save')sessionStorage.setItem('test-config-writes',String(Number(sessionStorage.getItem('test-config-writes'))+1));
                post(q);
            }});
            b.addEventListener('message',({data:q})=>{
                byRequest.set(q.request,q);
                if(q.event==='settings.snapshot' && q.state==='ready')window.configSample=q;
                if(q.event==='settings.result')historyOfSaves.push(q);
            });
            el('board-settings').click();return true;
        })())JS");
        expect("actual editable timeout and read-only provider","window.configSample?.canSave && document.querySelector('.host-settings')?.dataset.settingsState==='ready' && "
            "el('host-provider').textContent==='deepseek' && el('host-timeout-input')?.type==='number' && !document.querySelector('#host-provider input')");
    };
    open();
    if (mode==L"settings-restart") {
        Json original, later;
        std::ifstream(data/"host-first-saved.json") >> original;
        std::ifstream(data/"host-next-saved.json") >> later;
        evaluate("inspect the two original persistent receipts","window.earlier="+original.dump()+";window.later="+later.dump()+R"JS(;
            for(const [request,old] of [['old-config',earlier],['latest-config',later]])chrome.webview.postMessage({
                type:'settings.receipt',version:'1',owner:'host',instance:configSample.instance,request,submission:old.submission});
            true)JS");
        expect("restarted resident adopted the final configuration and kept both original records","configSample.instance!==earlier.instance && "
            "configSample.appliedRevision===later.savedRevision && !configSample.pendingRestart && el('applied-timeout').textContent.includes('23000') && "
            "el('saved-timeout').textContent.includes('23000') && byRequest.get('old-config')?.savedValue===21000 && "
            "byRequest.get('old-config').savedRevision===earlier.savedRevision && byRequest.get('latest-config')?.savedValue===23000 && !sessionStorage.getItem('test-config-writes')");
        next(); return;
    }
    if (mode==L"settings-conflict") {
        wait("host-writer-ready");
        evaluate("hold a genuine earlier edit","window.holdConfig=true; editTimeout('23000'); true");
        expect("draft can be saved at first revision","el('save-host-timeout').disabled===false");
        save("host-draft-pinned","configSample.revision");
        wait("host-first-saved.json");
        evaluate("submit from the original draft revision","el('save-host-timeout').click(); true");
        expect("host returns conflict and keeps the actual typed draft","historyOfSaves.some(q=>q.state==='conflict') && el('host-timeout-input').value==='23000'");
        evaluate("release this window's original reads","window.holdConfig=false; heldConfig.splice(0).forEach(q=>chrome.webview.postMessage(q)); el('refresh-host-settings').click(); true");
        expect("fresh revision cannot overwrite the draft or automatically submit it","el('saved-timeout').textContent.includes('21000') && "
            "el('applied-timeout').textContent.includes('17000') && !!el('rebase-host-timeout') && el('save-host-timeout').disabled && el('host-timeout-input').value==='23000'");
        evaluate("explicitly keep the draft at the new revision","el('rebase-host-timeout').click(); true");
        expect("rebased draft is ready for new explicit save","!el('save-host-timeout').disabled && !el('rebase-host-timeout')");
        evaluate("explicit save of rebased value","el('save-host-timeout').click(); true");
        expect("second save still awaits resident restart","historyOfSaves.some(q=>q.state==='saved' && q.savedValue===23000) && configSample.pendingRestart && "
            "el('saved-timeout').textContent.includes('23000') && el('applied-timeout').textContent.includes('17000') && sessionStorage.getItem('test-config-writes')==='2'");
        save("host-next-saved.json","historyOfSaves.find(q=>q.state==='saved' && q.savedValue===23000)");
        next(); return;
    }
    evaluate("enter a rejected range","editTimeout('600001'); true");
    expect("invalid range is never submitted","el('save-host-timeout').disabled && el('host-timeout-input').getAttribute('aria-invalid')==='true'");
    evaluate("enter actual desired timeout","editTimeout('21000'); true");
    expect("valid new value enables explicit save","!el('save-host-timeout').disabled");
    save("host-writer-ready","configSample.instance");
    wait("host-draft-pinned");
    evaluate("one explicit double click saves once","const save=el('save-host-timeout');save.click();save.click();sessionStorage.getItem('test-config-writes')==='1'");
    expect("saved and applied values differ until restart","historyOfSaves.some(q=>q.state==='saved' && q.savedValue===21000) && "
        "document.querySelector('.settings-receipt')?.dataset.saveState==='saved' && el('saved-timeout').textContent.includes('21000') && "
        "el('applied-timeout').textContent.includes('17000') && configSample.pendingRestart");
    save("host-first-saved.json","historyOfSaves.find(q=>q.state==='saved' && q.savedValue===21000)");
    wait("host-next-saved.json");
    call("reload after peer has saved a later value",L"Page.reload",Json::object());
    expect("new settings document","performance.getEntriesByType('navigation')[0]?.type==='reload' && !window.historyOfSaves && !!document.querySelector('#board-settings')");
    open();
    expect("original saved receipt is distinct from peer's newer setting","historyOfSaves.some(q=>q.state==='saved' && q.savedValue===21000 && q.savedRevision!==configSample.revision) && "
        "el('saved-timeout').textContent.includes('23000') && el('applied-timeout').textContent.includes('17000') && sessionStorage.getItem('test-config-writes')==='1'");
    next();
}
}
