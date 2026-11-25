#include "../app/gui/interaction_probe.hpp"
#include "ui_main_panel_fixture.hpp"
#include <algorithm>
#include <cstdio>

namespace dfy::tests {
// 构造空插件应用，固定模型可阻塞至原生验收放行并检查清空后的真实请求。
std::unique_ptr<app::Application> open_panel_fixture(const std::filesystem::path& work, std::shared_ptr<UiDialogueGate> gate) {
    auto application = std::make_unique<app::Application>(model::Config{},work/"plugins",work/"user",model::Backend{
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
            if (text == "窗口补充，保持同一轮") {
                const auto count = request.messages.size();
                if (count<3 || request.messages[count-3].content!="等待桌面停止" ||
                    request.messages[count-2].role!=model::Role::assistant || request.messages[count-2].content!="固定回复：等待桌面停止")
                    throw std::runtime_error("UI fixture missing original same-turn context");
            }
            return {{model::Role::assistant,"固定回复："+text}};
        }});
    const auto layout = application->room().layout();
    gate->room = layout.room.instance;
    gate->body = application->room().self(layout.room)->body;
    return application;
}
}

namespace dfy::ui {
// 通过正式页面读取原生房间，核对显式退出和意外退出的不同回执。
// 步骤：等待真实后台数据 → 核对本窗日志和目录操作 → 按场景提交退出或观察原进程丢失。
void InteractionProbe::start_observer(bool stop, bool log_error, const std::filesystem::path& replacement_ready) {
    expect("resident observer ready","document.querySelector('#dialogue-state')?.dataset.accepting==='true' && document.querySelector('#room-location')?.textContent==='书房 · 书桌旁'");
    evaluate("inspect resident settings","document.querySelector('#board-settings').click(); true");
    expect("window log status",log_error ? "document.querySelector('#log-status')?.textContent.includes('写入日志失败，系统错误码')" :
        "document.querySelector('#log-status')?.textContent.includes('已写入运行日志')");
    evaluate("open configured log folder","document.querySelector('#open-logs').click(); true");
    expect("log folder result",log_error ? "document.querySelector('#log-status')?.textContent.includes('无法打开日志目录，系统错误码 267')" :
        "document.querySelector('#log-status')?.textContent.includes('系统已接收打开目录请求')");
    evaluate("inspect stop confirmation","document.querySelector('#host-stop').click(); true");
    expect("confirmation visible","document.querySelector('#host-stop-title').closest('dialog').open");
    evaluate("leave resident running","document.querySelector('#host-stay').click(); !document.querySelector('#host-stop-title').closest('dialog').open");
    if (stop) {
        evaluate("request explicit completion","document.querySelector('#host-stop').click(); document.querySelector('#host-confirm-stop').click(); true");
    }
    if (!replacement_ready.empty()) {
        wait_for("fixture may end its own resident",[] { std::fputs("Observer ready for unexpected exit\n",stderr); std::fflush(stderr); return true; });
        expect("unexpected exit is visible with diagnostics","document.querySelector('#dialogue-state').textContent==='本体意外退出' && "
            "document.querySelector('#host-shutdown-status').textContent.includes('退出码') && document.querySelector('#host-stop').disabled");
        evaluate("retain original process evidence","sessionStorage.setItem('fixture-original-exit',document.querySelector('#host-shutdown-status').textContent.trim()); true");
        wait_for("replacement has acquired the old directory",[replacement_ready] { return std::filesystem::is_regular_file(replacement_ready); });
        call("reload with replacement resident online",L"Page.reload",Json::object());
        expect("retained original peer after replacement and reload","document.querySelector('#dialogue-state')?.textContent==='本体意外退出' && "
            "document.querySelector('#host-shutdown-status').textContent.trim()===sessionStorage.getItem('fixture-original-exit') && document.querySelector('#host-stop').disabled");
        evaluate("inspect settings after loss","document.querySelector('#board-settings').click(); true");
        evaluate("open local log after loss","document.querySelector('#open-logs').click(); true");
        expect("local log is still available","document.querySelector('#log-status')?.textContent.includes('系统已接收打开目录请求')");
    }
    next();
}

// 在空插件正式会话上验证真实输入与按原回合停止、清空的执行结果。
// 步骤：发送原话并比对回执 → 核对来源、失败与刷新 → 阻塞原请求并停止 → 放行迟到结果并核实清空。
void InteractionProbe::start_main_panel(std::shared_ptr<const FrameChecks> checks, std::shared_ptr<tests::UiDialogueGate> gate) {
    expect("main dialogue ready","document.querySelector('#dialogue-state')?.dataset.accepting === 'true'");
    expect("room position rendered","document.querySelector('#room-location')?.textContent==='书房 · 书桌旁'");
    evaluate("room and chat share a desktop row",R"JS((()=>{
        const room=document.querySelector('.room-view').getBoundingClientRect();
        const chat=document.querySelector('.chat-pane').getBoundingClientRect();
        return room.width>0 && chat.width>0 && room.right<=chat.left+1 && room.top===chat.top;
    })())JS");
    evaluate("visit settings with a draft",R"JS((()=>{
        const draft=document.querySelector('#dialogue-draft');
        draft.value='切换时保留的草稿'; draft.dispatchEvent(new Event('input'));
        document.querySelector('#board-settings').click(); return true;
    })())JS");
    expect("settings takes the full workspace","document.querySelector('#board-settings').getAttribute('aria-current')==='page' && "
        "!document.querySelector('.chat-pane').getClientRects().length && "
        "document.querySelector('.settings-view').getBoundingClientRect().right===innerWidth");
    evaluate("return to room","document.querySelector('#board-room').click(); true");
    expect("same draft on return","document.querySelector('#dialogue-draft').value==='切换时保留的草稿' && "
        "document.querySelector('#board-room').getAttribute('aria-current')==='page'");
    evaluate("clear test draft","const draft=document.querySelector('#dialogue-draft'); draft.value=''; draft.dispatchEvent(new Event('input')); true");
    call("enable document lifecycle events",L"Page.enable",Json::object());
    call("hold first room query in the reloaded document",L"Page.addScriptToEvaluateOnNewDocument",{{"source",R"JS(document.addEventListener('readystatechange',()=>{
        if (document.readyState!=='interactive' || location.origin!=='https://panel.dafeiyu.invalid' || sessionStorage.getItem('fixture-room-drop')) return;
        sessionStorage.setItem('fixture-room-drop','1');
        const bridge=window.chrome.webview, post=bridge.postMessage.bind(bridge);
        window.fixtureRoomReads=0;
        Object.defineProperty(bridge,'postMessage',{configurable:true,value:fields=>{
            if (fields.type==='room.read' && (++window.fixtureRoomReads===1 || window.fixtureHoldRoom)) return;
            post(fields);
        }});
    }))JS"}});
    call("reload before room read",L"Page.reload",Json::object());
    expect("lost room query is recovered without leaving the panel","window.fixtureRoomReads>=2 && "
        "document.querySelector('#room-location')?.textContent==='书房 · 书桌旁' && !document.querySelector('.turn')");
    expect("native room coordinates and activity are readable","document.querySelector('[data-source=room]')?.textContent.includes('(4, 0)') && "
        "document.querySelector('[data-source=room]').textContent.includes('停留') && !document.querySelector('.changes li')");
    evaluate("hold room updates and retain last confirmed position",R"JS((async()=>{
        window.fixtureHoldRoom=true;
        const end=performance.now()+6500;
        while (document.querySelector('[data-source=room]').dataset.current!=='false' && performance.now()<end)
            await new Promise(r=>setTimeout(r,50));
        return document.querySelector('[data-source=room]').textContent.includes('数据过期') &&
            document.querySelector('#room-location')?.textContent==='书房 · 书桌旁' &&
            document.querySelector('[data-source=conversation]').dataset.current==='true';
    })())JS");
    evaluate("resume real room queries","window.fixtureHoldRoom=false; true");
    expect("room snapshot is current again","document.querySelector('[data-source=room]').dataset.current==='true'");
    call("room matches original application body",L"Runtime.evaluate",{{"expression","({...document.querySelector('#room-location').dataset})"},{"returnByValue",true}},
        [gate](const Json& reply) {
            const auto& data = reply.at("result").at("value");
            if (data.at("room")!=std::to_string(gate->room.load()) || data.at("body")!=std::to_string(gate->body.load()))
                throw std::runtime_error("UI location differs from native room binding");
        });
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
    evaluate("opening room does not start a model turn","(async()=>{const s=await dialogueAsk('probe-idle',{type:'state.read',topic:'run'});return s.run===null;})()");
    const auto text = Json(prompt).dump();
    call("focus emulation",L"Emulation.setFocusEmulationEnabled",{{"enabled",true}});
    evaluate("focus composer","document.querySelector('#dialogue-draft').focus(); document.activeElement.id === 'dialogue-draft'");
    call("compose Chinese",L"Input.imeSetComposition",{{"text","你好"},{"selectionStart",2},{"selectionEnd",2}});
    evaluate("composition not submitted","!document.querySelector('.turn') && document.querySelector('#dialogue-send').disabled");
    call("commit entered prompt",L"Input.insertText",{{"text",prompt}});
    evaluate("send once on rapid double click","const button=document.querySelector('#dialogue-send'); button.click(); "
        "window.original=history.state.dialogue.commands[0]; button.click(); !!original.submission && Number.isSafeInteger(original.sourceUtcMs)");
    expect("one text answer with real exit","document.querySelectorAll('.turn').length === 1 && "
        "document.querySelector('.turn').dataset.exit === 'succeeded' && document.querySelector('.outgoing p').textContent === "+text+
        " && document.querySelector('.incoming p').textContent === "+Json("固定回复："+prompt).dump()+
        " && !document.querySelector('.incoming p b') && !document.querySelector('#dialogue-draft').value");
    evaluate("same command reuses real turn and durable message", R"JS((async()=>{
        const receipt=(await dialogueAsk('probe-repeat',original)).receipt;
        const {conversation}=await dialogueAsk('probe-subscription',{type:'messages.subscribe'});
        const {record}=await dialogueAsk('probe-stored',{type:'messages.receipt',conversation,source:'desktop',submission:original.submission});
        return receipt.state==='accepted' && receipt.turn===document.querySelector('.turn').dataset.turn &&
            receipt.storedMessage===record.message && record.text===original.text && record.sourceUtcMs===original.sourceUtcMs;
    })())JS");
    expect("changes are attributed to the observed conversation","[...document.querySelectorAll('.changes li')].some(item=>"
        "item.textContent.includes('最近回合：尚无回合 → ') && item.textContent.includes('主会话 · 修订'))");
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
    expect("failed round without exception text","document.querySelectorAll('.turn').length === 2 && document.querySelector(':nth-last-child(1 of .turn)').dataset.result === 'failed' && "
        "document.querySelector(':nth-last-child(1 of .turn) .turn-status').textContent === '本轮回复失败' && !document.body.textContent.includes('fixed private provider error')");
    call("open other top document",L"Page.navigate",{{"url",utf8(std::wstring(center_url)+L"#room-idle")}});
    expect("other local document loaded","!!document.querySelector('#room-draft')");
    evaluate("other top document cannot submit or read dialogue",R"JS((async()=>{
        const bridge = window.chrome.webview;
        const replies = new Map();
        bridge.addEventListener('message',({data})=>replies.set(data.request,data));
        bridge.postMessage({type:'dialogue.submit',version:'1',request:'other-write',command:'3',text:'wrong document'});
        bridge.postMessage({type:'dialogue.read',version:'1',request:'other-read',command:'',revision:''});
        bridge.postMessage({type:'room.read',version:'1',request:'other-room'});
        bridge.postMessage({type:'messages.subscribe',version:'1',request:'other-subscription'});
        bridge.postMessage({type:'messages.read',version:'1',request:'other-history',conversation:'main',direction:'history',cursor:'0',through:'0',limit:24});
        bridge.postMessage({type:'messages.receipt',version:'1',request:'other-receipt',conversation:'main',source:'desktop',submission:'previous'});
        for (const kind of ['cancel','clear']) bridge.postMessage({type:'dialogue.'+kind,version:'1',request:'other-'+kind,command:'3',session:'1',turn:'1'});
        for (const kind of ['append','enqueue']) bridge.postMessage({type:'dialogue.'+kind,version:'1',request:'other-'+kind,command:'3',session:'1',turn:'1',text:'wrong document'});
        for (const kind of ['remove','resume']) bridge.postMessage({type:'dialogue.'+kind,version:'1',request:'other-'+kind,command:'3',session:'1',message:'1'});
        bridge.postMessage({type:'state.read',version:'1',request:'other-barrier',topic:'run'});
        const end=performance.now()+1800;
        while (!replies.has('other-barrier') && performance.now()<end) await new Promise(r=>setTimeout(r,20));
        return location.origin==='https://center.dafeiyu.invalid' && !history.state?.dialogue &&
            replies.get('other-barrier')?.run?.result === 'failed' &&
            ['write','read','room','subscription','history','receipt','cancel','clear','append','enqueue','remove','resume'].every(key=>!replies.has('other-'+key));
    })())JS");
    call("return to dialogue document",L"Page.navigate",{{"url",utf8(main_panel_url)}});
    expect("return preserves only the accepted turns","document.querySelectorAll('.turn').length === 2 && !!document.querySelector('.incoming p')");
    call("reload same main document",L"Page.reload",Json::object());
    expect("reload restores same completed turns","document.querySelectorAll('.turn').length === 2 && document.querySelector('.incoming p').textContent.startsWith('固定回复：') && "
        "document.querySelector(':nth-last-child(1 of .turn)').dataset.result === 'failed'");
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
    expect("cancelled output precedes exit","document.querySelector(':nth-last-child(1 of .turn)')?.dataset.result==='cancelled' && "
        "document.querySelector(':nth-last-child(1 of .turn)').dataset.exit==='pending' && document.querySelector('#dialogue-state').dataset.accepting==='false'");
    wait_for("release late fixed reply",[gate] { gate->release.release(); return true; });
    expect("original cancellation survives late reply","document.querySelector('#dialogue-state').dataset.accepting==='true' && "
        "document.querySelector(':nth-last-child(1 of .turn)').dataset.result==='cancelled' && document.querySelector(':nth-last-child(1 of .turn)').dataset.exit!=='pending' && !document.querySelector(':nth-last-child(1 of .turn) .incoming')");
    evaluate("clear exact idle history and repeat original receipt",R"JS((async()=>{
        return (await control('clear','7')).state==='accepted' && (await control('clear','7')).state==='accepted';
    })())JS");
    expect("cleared display","document.querySelectorAll('.turn').length===0");
    evaluate("send from cleared history",R"JS((async()=>{
        return (await panelCommand({type:'dialogue.submit',command:'8',text:'检查清空后的上下文'})).state==='accepted';
    })())JS");
    expect("new context answers while durable history remains","document.querySelectorAll('.turn').length===1 && "
        "document.querySelector('.turn .incoming p')?.textContent==='固定回复：检查清空后的上下文' && "
        "[...document.querySelectorAll('.history-message p')].some(p=>p.textContent===" + text + ")");
    evaluate("old clear cannot erase new dialogue",R"JS((async()=>{
        return (await control('clear','7')).state==='accepted' && (await control('clear','9')).state==='stale';
    })())JS");
    expect("new dialogue remains visible","document.querySelectorAll('.turn').length===1 && !!document.querySelector('.incoming p')");
    call("reopen functional controls",L"Page.reload",Json::object());
    expect("reloaded composer and reset are available","document.querySelector('#dialogue-state')?.dataset.accepting==='true' && "
        "document.querySelector('#dialogue-clear')?.disabled===false");
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
        "document.querySelector(':nth-last-child(1 of .turn) .turn-status').textContent==='已停止回复' && document.querySelector('#dialogue-stop').disabled");
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
        "document.querySelector('#room-location')?.textContent==='书房 · 书桌旁' && document.querySelector('#dialogue-draft').value==='留给下一轮' && document.activeElement.id==='dialogue-draft'");
    check_dialogue_queue(gate);
    check_dialogue_recovery();
    evaluate("dialogue bridge honors application byte limits",R"JS((async()=>{
        const bridge=window.chrome.webview, replies=new Map();
        const collect=({data})=>{ if(data.request?.startsWith('size-')) replies.set(data.request,data); };
        bridge.addEventListener('message',collect);
        let serial=0;
        // 从真实文档取得一条限定时间内返回的宿主消息。
        async function ask(fields) {
            const request='size-'+ ++serial;
            bridge.postMessage({...fields,version:'1',request});
            const end=performance.now()+1800;
            while(!replies.has(request) && performance.now()<end) await new Promise(r=>setTimeout(r,10));
            return replies.get(request);
        }
        try {
            const read={type:'dialogue.read',command:'',revision:''};
            let command=BigInt((await ask(read)).latestCommand)+1n;
            const excess=await ask({type:'dialogue.submit',command:String(command++),text:'x'.repeat(1024*1024+1)});
            if(excess?.receipt?.state!=='rejected') return false;
            const text=('窗边\n"\u0001').repeat(10000), id=String(command);
            const accepted=await ask({type:'dialogue.submit',command:id,text});
            if(!['queued','accepted'].includes(accepted?.receipt?.state)) return false;
            const end=performance.now()+2500;
            while(performance.now()<end) {
                const s=await ask({...read,command:id}), turn=s?.turns?.at(-1);
                if(turn?.prompt===text && turn.exit==='succeeded')
                    return s.receipt?.turn===turn.turn && turn.answer==='固定回复：'+text;
            }
            return false;
        } finally { bridge.removeEventListener('message',collect); }
    })())JS");
    next();
}

// 让原回合实际保持在途，核对设置中的退出进度与等待提示。
// 步骤：启动并确认原回合 → 通过设置退出 → 核对等待对象和持续时间 → 放行原工作。
void InteractionProbe::start_panel_shutdown(std::shared_ptr<tests::UiDialogueGate> gate) {
    expect("ready to start shutdown fixture","document.querySelector('#dialogue-state')?.dataset.accepting==='true'");
    evaluate("start blocked turn for host shutdown",R"JS((async()=>{
        const draft=document.querySelector('#dialogue-draft'); draft.value='等待桌面停止'; draft.dispatchEvent(new Event('input'));
        await Promise.resolve(); document.querySelector('#dialogue-send').click(); return true;
    })())JS");
    wait_for("fixed model is processing original turn",[gate] { return gate->entered.load()==1; });
    expect("record original active turn","document.querySelector(':nth-last-child(1 of .turn)')?.dataset.exit==='pending' && "
        "(window.fixtureStopTurn=document.querySelector(':nth-last-child(1 of .turn)').dataset.turn)");
    evaluate("confirm shutdown during active turn","document.querySelector('#board-settings').click(); document.querySelector('#host-stop').click(); document.querySelector('#host-confirm-stop').click(); true");
    expect("show verified original turn during shutdown","document.querySelector('#host-shutdown-status')?.textContent.includes('的回合 '+window.fixtureStopTurn+' 退出')");
    evaluate("show long wait while the worker is still in flight",R"JS((async()=>{
        const end=performance.now()+12000;
        while(performance.now()<end) {
            if(document.querySelector('#host-shutdown-status').textContent.includes('已等待 10 秒以上')) return true;
            await new Promise(resolve=>setTimeout(resolve,100));
        }
        return false;
    })())JS");
    wait_for("release original turn after progress is visible",[gate] { gate->release.release(); return true; });
    next();
}
}
