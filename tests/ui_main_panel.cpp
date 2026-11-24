#include "../app/gui/interaction_probe.hpp"
#include "ui_main_panel_fixture.hpp"
#include <algorithm>

namespace dfy::tests {
// 构造空插件应用，固定模型可阻塞至原生验收放行并检查清空后的真实请求。
std::unique_ptr<app::Application> open_panel_fixture(const std::filesystem::path& work, std::shared_ptr<UiDialogueGate> gate) {
    return std::make_unique<app::Application>(model::Config{},work/"plugins",work/"user",model::Backend{
        // 依据约定原话制造失败或在途答复，并核对后继请求使用的上下文。
        // 步骤：选择固定响应 → 按场景等待放行或核验历史 → 归还实际答复。
        [gate](const auto&, const auto& request) -> model::Response {
            const auto& text = request.messages.back().content;
            if (text == "模拟请求失败") throw std::runtime_error("fixed private provider error");
            if (text == "等待桌面停止") {
                ++gate->entered;
                if (!gate->release.try_acquire_for(std::chrono::seconds(15))) throw std::runtime_error("UI fixture gate timed out");
            }
            if (text == "检查清空后的上下文" && std::ranges::count_if(request.messages,[](const auto& message) {
                return message.role == model::Role::user || message.role == model::Role::assistant;
            }) != 1) throw std::runtime_error("UI fixture found prior dialogue");
            return {{model::Role::assistant,"固定回复："+text}};
        }});
}
}

namespace dfy::ui {
// 在空插件正式会话上验证真实输入与按原回合停止、清空的执行结果。
// 步骤：发送原话并比对回执 → 核对来源、失败与刷新 → 阻塞原请求并停止 → 放行迟到结果并核实清空。
void InteractionProbe::start_main_panel(std::shared_ptr<const FrameChecks> checks, std::shared_ptr<tests::UiDialogueGate> gate) {
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
        for (const kind of ['cancel','clear']) bridge.postMessage({type:'dialogue.'+kind,version:'1',request:'other-'+kind,command:'3',session:'1',turn:'1'});
        bridge.postMessage({type:'state.read',version:'1',request:'other-barrier',topic:'run'});
        const end=performance.now()+1800;
        while (!replies.has('other-barrier') && performance.now()<end) await new Promise(r=>setTimeout(r,20));
        return replies.get('other-barrier')?.run?.result === 'failed' && ['write','read','cancel','clear'].every(key=>!replies.has('other-'+key));
    })())JS");
    call("return to dialogue document",L"Page.navigate",{{"url",utf8(main_panel_url)}});
    expect("return preserves only the accepted turns","document.querySelectorAll('.turn').length === 2 && !!document.querySelector('.incoming p')");
    call("reload same main document",L"Page.reload",Json::object());
    expect("reload restores same completed turns","document.querySelectorAll('.turn').length === 2 && document.querySelector('.incoming p').textContent.startsWith('固定回复：') && "
        "document.querySelector('.turn:last-child').dataset.result === 'failed'");
    evaluate("install numbered command checks",R"JS((()=>{
        const bridge=window.chrome.webview, replies=new Map();
        let sequence=0;
        bridge.addEventListener('message',({data})=>{ if(data.request?.startsWith('control-')) replies.set(data.request,data); });
        // 在同一受信文档上获取一次真实请求回包。
        async function ask(fields) {
            const request='control-'+ ++sequence;
            bridge.postMessage({...fields,version:'1',request});
            const end=performance.now()+2000;
            while(!replies.has(request) && performance.now()<end) await new Promise(r=>setTimeout(r,10));
            return replies.get(request);
        }
        // 等待原命令的业务回执，排队期间只读不重投。
        window.panelCommand=async fields=>{
            let {receipt}=await ask(fields);
            const end=performance.now()+2000;
            while(receipt?.state==='queued' && performance.now()<end)
                ({receipt}=await ask({type:'dialogue.read',command:fields.command,revision:''}));
            return receipt;
        };
        return true;
    })())JS");
    evaluate("submit controlled slow round",R"JS((async()=>{
        const r=await panelCommand({type:'dialogue.submit',command:'3',text:'等待桌面停止'});
        window.held={session:r.session,turn:r.turn};
        window.control=(kind,id,target=held)=>panelCommand({type:'dialogue.'+kind,command:id,...target});
        return r.state==='accepted';
    })())JS");
    wait_for("fixed backend holds original request",[gate] { return gate->entered.load() == 1; });
    evaluate("reject stale stop and busy clear",R"JS((async()=>{
        const stale=await control('cancel','4',{session:held.session,turn:'1'});
        const busy=await control('clear','5');
        return stale.state==='stale' && busy.state==='busy';
    })())JS");
    evaluate("reuse stop receipt for the original turn",R"JS((async()=>{
        const r=await control('cancel','6'), same=await control('cancel','6'), conflict=await control('clear','6');
        return r.state==='accepted' && r.turn===held.turn && same.turn===held.turn && same.state==='accepted' && conflict.state==='conflict';
    })())JS");
    expect("cancelled output precedes exit","document.querySelector('.turn:last-child')?.dataset.result==='cancelled' && "
        "document.querySelector('.turn:last-child').dataset.exit==='pending' && document.querySelector('#dialogue-state').dataset.accepting==='false'");
    wait_for("release late fixed reply",[gate] { gate->release.release(); return true; });
    expect("original cancellation survives late reply","document.querySelector('#dialogue-state').dataset.accepting==='true' && "
        "document.querySelector('.turn:last-child').dataset.result==='cancelled' && document.querySelector('.turn:last-child').dataset.exit!=='pending' && !document.querySelector('.turn:last-child .incoming')");
    evaluate("clear exact idle history and repeat original receipt",R"JS((async()=>{
        return (await control('clear','7')).state==='accepted' && (await control('clear','7')).state==='accepted';
    })())JS");
    expect("cleared display","document.querySelectorAll('.turn').length===0");
    evaluate("send from cleared history",R"JS((async()=>{
        return (await panelCommand({type:'dialogue.submit',command:'8',text:'检查清空后的上下文'})).state==='accepted';
    })())JS");
    expect("new answer has no prior history","document.querySelectorAll('.turn').length===1 && document.querySelector('.incoming p')?.textContent==='固定回复：检查清空后的上下文'");
    evaluate("old clear cannot erase new dialogue",R"JS((async()=>{
        return (await control('clear','7')).state==='accepted' && (await control('clear','9')).state==='stale';
    })())JS");
    expect("new dialogue remains visible","document.querySelectorAll('.turn').length===1 && !!document.querySelector('.incoming p')");
    call("reopen functional controls",L"Page.reload",Json::object());
    expect("history offers clear","document.querySelector('#dialogue-clear')?.disabled===false");
    evaluate("open real clear confirmation","document.querySelector('#dialogue-clear').click(); document.querySelector('dialog').open");
    key("dismiss clear with escape","Escape",27);
    expect("dismissal leaves history","!document.querySelector('dialog').open && document.querySelectorAll('.turn').length===1");
    call("narrow composer",L"Emulation.setDeviceMetricsOverride",{{"width",320},{"height",560},{"deviceScaleFactor",1},{"mobile",false}});
    evaluate("submit slow dialogue through the composer",R"JS((()=>{
        const draft=document.querySelector('#dialogue-draft');
        draft.value='等待桌面停止'; draft.dispatchEvent(new Event('input'));
        return Promise.resolve().then(()=>{document.querySelector('#dialogue-send').click(); return true;});
    })())JS");
    wait_for("second fixed reply is in flight",[gate] { return gate->entered.load() == 2; });
    expect("narrow viewport keeps stop available","document.querySelector('#dialogue-stop')?.disabled===false && "
        "document.querySelector('#dialogue-stop').getBoundingClientRect().bottom<=innerHeight && document.documentElement.scrollWidth<=innerWidth && document.querySelector('#dialogue-clear').disabled");
    evaluate("preserve a new draft and stop via the button",R"JS((()=>{
        const draft=document.querySelector('#dialogue-draft');
        draft.value='留给下一轮'; draft.dispatchEvent(new Event('input'));
        document.querySelector('#dialogue-stop').click(); return true;
    })())JS");
    expect("visible closing state while worker still holds the call","document.querySelector('#dialogue-state').textContent==='正在收尾' && "
        "document.querySelector('.turn:last-child .turn-status').textContent==='已停止回复' && document.querySelector('#dialogue-stop').disabled");
    wait_for("release second cancelled reply",[gate] { gate->release.release(); return true; });
    expect("actual exit enables idle controls","!document.querySelector('#dialogue-stop') && document.querySelector('#dialogue-clear')?.disabled===false && "
        "document.querySelector('#dialogue-state').textContent==='可以聊天' && document.querySelector('#dialogue-draft').value==='留给下一轮'");
    evaluate("confirm clear with a retained draft",R"JS((()=>{
        document.querySelector('#dialogue-clear').click();
        const box=document.querySelector('dialog');
        if (!box.open || !box.textContent.includes('草稿会保留')) return false;
        document.querySelector('#dialogue-confirm-clear').click(); return true;
    })())JS");
    expect("clear removes history but preserves editable draft","!document.querySelector('.turn') && !document.querySelector('dialog').open && "
        "document.querySelector('.room-still img')?.naturalWidth>0 && document.querySelector('#dialogue-draft').value==='留给下一轮' && document.activeElement.id==='dialogue-draft'");
    next();
}
}
