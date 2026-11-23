#include "../app/gui/interaction_probe.hpp"

namespace dfy::ui {
// 在空插件正式会话上验证真实输入、回复和文档隔离，模型只使用固定响应。
// 步骤：通过页面发送原话 → 比对快照与重复回执 → 拒绝其他文档 → 检查失败与刷新恢复。
void InteractionProbe::start_main_panel(std::shared_ptr<const FrameChecks> checks) {
    expect("main dialogue ready","document.querySelector('#dialogue-state')?.dataset.accepting === 'true'");
    evaluate("observe dialogue bridge replies",R"JS((()=>{
        const bridge = window.chrome.webview;
        const replies = new Map();
        bridge.addEventListener('message',({data})=>{ if(data.request?.startsWith('probe-')) replies.set(data.request,data); });
        window.dialogueReplies = replies;
        window.dialoguePost = (request, fields) => bridge.postMessage({ ...fields, request, version:'1' });
        window.dialogueAsk = async (request, fields) => {
            window.dialoguePost(request, fields);
            const end = performance.now() + 1800;
            while (!replies.has(request) && performance.now() < end) await new Promise(r=>setTimeout(r,20));
            return replies.get(request);
        };
        return true;
    })())JS");
    const std::string prompt = "你好，大肥鱼 <b>验证</b>";
    const auto text = Json(prompt).dump();
    call("focus emulation",L"Emulation.setFocusEmulationEnabled",{{"enabled",true}});
    evaluate("focus composer","document.querySelector('#dialogue-draft').focus(); document.activeElement.id === 'dialogue-draft'");
    call("compose Chinese",L"Input.imeSetComposition",{{"text","你好"},{"selectionStart",2},{"selectionEnd",2}});
    evaluate("composition not submitted","!document.querySelector('.turn') && document.querySelector('#dialogue-send').disabled");
    call("commit entered prompt",L"Input.insertText",{{"text",prompt}});
    evaluate("send once on rapid double click","const button=document.querySelector('#dialogue-send'); button.click(); button.click(); true");
    expect("one text answer with real exit","document.querySelectorAll('.turn').length === 1 && "
        "document.querySelector('.turn').dataset.exit === 'succeeded' && document.querySelector('.outgoing p').textContent === "+text+
        " && document.querySelector('.incoming p').textContent === "+Json("固定回复："+prompt).dump()+
        " && !document.querySelector('.incoming p b') && !document.querySelector('#dialogue-draft').value");
    evaluate("same command reuses real turn", "(async()=>{const receipt=(await window.dialogueAsk('probe-repeat',"
        "{type:'dialogue.submit',command:'1',text:"+text+"})).receipt; return receipt.state==='accepted' && "
        "receipt.turn===document.querySelector('.turn').dataset.turn;})()");
    evaluate("different text conflicts",R"JS((async()=>{
        const r = await window.dialogueAsk('probe-conflict',{type:'dialogue.submit',command:'1',text:'conflicting text'});
        return r.receipt.state === 'conflict';
    })())JS");
    evaluate("public status contains no dialogue text","(async()=>{ const r=await window.dialogueAsk('probe-run',"
        "{type:'state.read',topic:'run'}); return r.run.result==='succeeded' && !JSON.stringify(r).includes("+text+"); })()");
    evaluate("create separate probe document",R"JS((()=>{
        const bridge = window.chrome.webview;
        bridge.addEventListener('message',({data})=>{
            if(data.event !== 'frame.opened') return;
            const frame=document.createElement('iframe'); frame.src=data.url+'#verify'; document.body.append(frame);
        });
        bridge.postMessage({type:'frame.open'});
        return true;
    })())JS");
    wait_for("child cannot submit or read dialogue",[checks] {
        return checks->decisions[4] == 4 && checks->verified();
    });
    evaluate("remove probe document","document.querySelector('iframe').remove(); true");
    wait_for("separate document exited",[checks] { return checks->destroyed == 1 && !checks->active; });
    evaluate("enter a fixed failure request","const box=document.querySelector('#dialogue-draft'); box.focus(); box.value='模拟请求失败'; box.dispatchEvent(new Event('input')); true");
    key("send failure using Enter","Enter",13);
    expect("failed round without exception text","document.querySelectorAll('.turn').length === 2 && document.querySelector('.turn:last-child').dataset.result === 'failed' && "
        "document.querySelector('.turn:last-child .turn-status').textContent === '本轮回复失败' && !document.body.textContent.includes('fixed private provider error')");
    call("open other top document",L"Page.navigate",{{"url",utf8(std::wstring(center_url)+L"#room-idle")}});
    expect("other local document loaded","!!document.querySelector('#room-draft')");
    evaluate("other top document cannot submit or read dialogue",R"JS((async()=>{
        const bridge = window.chrome.webview;
        const replies = new Map();
        bridge.addEventListener('message',({data})=>replies.set(data.request,data));
        bridge.postMessage({type:'dialogue.submit',version:'1',request:'other-write',command:'3',text:'wrong document'});
        bridge.postMessage({type:'dialogue.read',version:'1',request:'other-read',command:'',revision:''});
        bridge.postMessage({type:'state.read',version:'1',request:'other-barrier',topic:'run'});
        const end=performance.now()+1800;
        while (!replies.has('other-barrier') && performance.now()<end) await new Promise(r=>setTimeout(r,20));
        return replies.get('other-barrier')?.run?.result === 'failed' && !replies.has('other-write') && !replies.has('other-read');
    })())JS");
    call("return to dialogue document",L"Page.navigate",{{"url",utf8(main_panel_url)}});
    expect("return preserves only the accepted turns","document.querySelectorAll('.turn').length === 2 && !!document.querySelector('.incoming p')");
    call("reload same main document",L"Page.reload",Json::object());
    expect("reload restores same completed turns","document.querySelectorAll('.turn').length === 2 && document.querySelector('.incoming p').textContent.startsWith('固定回复：') && "
        "document.querySelector('.turn:last-child').dataset.result === 'failed'");
    next();
}
}
