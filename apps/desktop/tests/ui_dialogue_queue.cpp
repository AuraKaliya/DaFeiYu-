#include "../src/interaction_probe.hpp"
#include "ui_main_panel_fixture.hpp"

namespace dfy::ui {
// 在同一受信页面核验队列命令的接纳、独立停止槽位和原消息到新回合的对应。
// 步骤：阻塞正式回合 → 压满队列并校验重投 → 停止后移除和恢复 → 读回各新轮。
void InteractionProbe::check_dialogue_queue(std::shared_ptr<tests::UiDialogueGate> gate) {
    evaluate("attach trusted queue client",R"JS((async()=>{
        const bridge=window.chrome.webview, replies=new Map();
        let requestId=0;
        bridge.addEventListener('message',({data})=>{ if(data.request?.startsWith('queue-')) replies.set(data.request,data); });
        // 只读回当前文档主动发出的原请求。
        async function ask(fields) {
            const request='queue-'+ ++requestId;
            bridge.postMessage({...fields,version:'1',request});
            const end=performance.now()+1800;
            while(!replies.has(request) && performance.now()<end) await new Promise(r=>setTimeout(r,5));
            if (!replies.has(request)) throw Error('queue request timed out');
            return replies.get(request);
        }
        const read=(command='',revision='')=>ask({type:'dialogue.read',command,revision});
        let latest=BigInt((await read()).latestCommand);
        // 只提交一次，沿原命令身份等到业务线程给出最终回执。
        async function command(kind, fields, id=String(++latest)) {
            let {receipt}=await ask({type:'dialogue.'+kind,command:id,...fields});
            const end=performance.now()+1800;
            while(receipt.state==='queued' && performance.now()<end) ({receipt}=await read(id));
            return receipt;
        }
        const active=await command('submit',{text:'等待桌面停止'});
        window.queueProbe={ask,read,command,active:{session:active.session,turn:active.turn},items:[]};
        return active.state==='accepted';
    })())JS");
    wait_for("queue predecessor entered backend",[gate] { return gate->entered.load() == 3; });
    evaluate("original append command keeps one waiting message",R"JS((async()=>{
        const {active,command,read}=queueProbe;
        const fields={...active,text:'只补充在原轮'};
        const first=await command('append',fields), same=await command('append',fields,first.command);
        const conflict=await command('enqueue',fields,first.command), stale=await command('append',{...fields,turn:'99999'});
        const inputs=(await read()).turns.at(-1).inputs;
        return first.state==='accepted' && first.turn===active.turn && same.message===first.message && conflict.state==='conflict' &&
            stale.state==='stale' && inputs.length===1 && inputs[0].message===first.message && inputs[0].state==='waiting' && inputs[0].modelStep==='0';
    })())JS");
    evaluate("full queue retains identities and private content",R"JS((async()=>{
        const {active,command,items,read,ask}=queueProbe;
        for(let i=0;i<8;i++) {
            const text=i<2?'排队中的同一句':'可移除 '+i;
            const r=await command('enqueue',{...active,text});
            if(r.state!=='accepted' || r.message==='0') return false;
            items.push(r);
        }
        const same=await command('enqueue',{...active,text:'排队中的同一句'},items[0].command);
        const conflict=await command('enqueue',{...active,text:'改写'},items[0].command);
        const snapshot=await read(), unchanged=await read('',snapshot.revision);
        const publicRun=await ask({type:'state.read',topic:'run'});
        return same.message===items[0].message && conflict.state==='conflict' &&
            new Set(items.map(r=>r.message)).size===8 && snapshot.queue.length===8 && !snapshot.queuePaused &&
            unchanged.queue===null && !JSON.stringify(publicRun).includes('排队中的同一句');
    })())JS");
    evaluate("stop remains independently available on queue overflow",R"JS((async()=>{
        const {command,active}=queueProbe;
        const [overflow,stop]=await Promise.all([command('enqueue',{...active,text:'超额'}),command('cancel',active)]);
        return overflow.state==='full' && stop.state==='accepted';
    })())JS");
    expect("paused queue belongs to cancelled and still running request",R"JS((await (async()=>{
        const s=await queueProbe.read();
        const turn=s.turns.at(-1);
        return s.queuePaused && s.queue.length===8 && turn.result==='cancelled' && turn.exit==='pending' &&
            turn.inputs[0].state==='cancelled' && turn.inputs[0].modelStep==='0';
    })()))JS");
    evaluate("remove by original message and resume exact head only",R"JS((async()=>{
        const {command,active,items}=queueProbe;
        const fields=message=>({session:active.session,message});
        if ((await command('remove',{session:'999999',message:items[0].message})).state!=='stale' ||
            (await command('resume',fields(items[1].message))).state!=='stale') return false;
        for(const item of items.slice(2)) {
            const r=await command('remove',fields(item.message));
            if(r.state!=='accepted' || (await command('remove',fields(item.message),r.command)).state!=='accepted') return false;
        }
        if((await command('resume',fields(items[0].message))).state!=='accepted') return false;
        const snapshot=await queueProbe.read();
        return snapshot.queue.length===2 && snapshot.turns.at(-1).exit==='pending';
    })())JS");
    wait_for("release queue predecessor",[gate] { gate->release.release(); return true; });
    expect("queued origins correlate to two new independent turns",R"JS((await (async()=>{
        const {items,read}=queueProbe, snapshot=await read();
        const started=snapshot.turns.filter(t=>items.some(r=>r.message===t.queuedMessage));
        return snapshot.queue.length===0 && started.length===2 && started[0].turn!==started[1].turn &&
            started.every((t,i)=>t.queuedMessage===items[i].message && t.exit==='succeeded' && t.answer==='固定回复：排队中的同一句');
    })()))JS");

    expect("composer is available for another round","document.querySelector('#dialogue-state').dataset.accepting==='true'");
    evaluate("start a held round from composer",R"JS((async()=>{
        const draft=document.querySelector('#dialogue-draft');
        draft.value='等待桌面停止'; draft.dispatchEvent(new Event('input')); await Promise.resolve();
        document.querySelector('#dialogue-send').click(); return true;
    })())JS");
    wait_for("composer round actually entered backend",[gate] { return gate->entered.load() == 4; });
    expect("busy input defaults to current run","document.querySelector('#dialogue-send').textContent==='追加本轮'");
    evaluate("focus current input","document.querySelector('#dialogue-draft').focus(); document.activeElement.id==='dialogue-draft'");
    call("input supplement before stop",L"Input.insertText",{{"text","留在本轮的补充"}});
    key("append using Enter","Enter",13);
    expect("supplement has a waiting receipt","document.querySelector(':nth-last-child(1 of .turn) .turn-input')?.dataset.state==='waiting' && !document.querySelector('#dialogue-draft').value");
    evaluate("choose next round explicitly","document.querySelector('input[name=destination][value=next]').click(); true");
    expect("busy primary control names next round","document.querySelector('#dialogue-send').textContent==='排到下一轮'");
    evaluate("focus actual queue composer","document.querySelector('#dialogue-draft').focus(); document.activeElement.id==='dialogue-draft'");
    call("input first waiting item",L"Input.insertText",{{"text","稍后聊窗边的书"}});
    key("queue first using Enter","Enter",13);
    expect("first waiting item with editable composer","document.querySelector('.queue-list li span')?.textContent==='稍后聊窗边的书' && !document.querySelector('#dialogue-draft').value");
    call("input second waiting item",L"Input.insertText",{{"text","可移除的消息"}});
    evaluate("queue by button","document.querySelector('#dialogue-send').click(); true");
    expect("both waiting items displayed","document.querySelectorAll('.queue-list li').length===2");
    call("reload while queue is waiting",L"Page.reload",Json::object());
    expect("reload restores both destinations","document.querySelectorAll('.queue-list li').length===2 && document.querySelector('#dialogue-stop')?.disabled===false && "
        "document.querySelector(':nth-last-child(1 of .turn) .turn-input p')?.textContent==='留在本轮的补充'");
    evaluate("remove second visible waiting item","document.querySelectorAll('.queue-remove')[1].click(); true");
    expect("removal preserves first waiting item","document.querySelectorAll('.queue-list li').length===1 && document.querySelector('.queue-list li span').textContent==='稍后聊窗边的书'");
    evaluate("stop original with remaining item","document.querySelector('#dialogue-stop').click(); true");
    expect("paused list and narrow input both remain reachable","document.querySelector('.queue-panel')?.dataset.paused==='true' && "
        "document.querySelector('#dialogue-resume')?.disabled===false && document.querySelector('#dialogue-state').textContent==='正在收尾' && "
        "document.documentElement.scrollWidth<=innerWidth && document.querySelector('#dialogue-send').getBoundingClientRect().bottom<=innerHeight");
    wait_for("release the held composer round",[gate] { gate->release.release(); return true; });
    expect("history can be cleared with paused queue","document.querySelector('#dialogue-clear')?.disabled===false");
    evaluate("restore undelivered input into an existing draft",R"JS((()=>{
        const box=document.querySelector('#dialogue-draft'), input=document.querySelector(':nth-last-child(1 of .turn) .turn-input');
        if(input.dataset.state!=='cancelled' || input.dataset.step!=='0') return false;
        box.value='保留原稿'; box.dispatchEvent(new Event('input')); input.querySelector('button').click(); return true;
    })())JS");
    expect("restored input is editable only","document.querySelector('#dialogue-draft').value==='保留原稿'+String.fromCharCode(10)+'留在本轮的补充' && document.activeElement.id==='dialogue-draft'");
    evaluate("clear confirmation describes waiting items",R"JS((()=>{
        document.querySelector('#dialogue-clear').click();
        if (!document.querySelector('dialog').textContent.includes('1 条待发消息也会保留')) return false;
        document.querySelector('#dialogue-confirm-clear').click(); return true;
    })())JS");
    expect("clear retains queue and original text","!document.querySelector('.turn') && document.querySelector('.queue-list li span')?.textContent==='稍后聊窗边的书'");
    expect("removed and cancelled originals retain distinct durable outcomes",R"JS((()=>{
        const states=[...document.querySelectorAll('.history-message [data-receipt]')];
        return states.some(s=>s.dataset.receipt==='removed' && s.parentElement.querySelector('p').textContent==='可移除的消息' && s.textContent.includes('已从待发队列移除')) &&
            states.some(s=>s.dataset.receipt==='cancelled' && s.parentElement.querySelector('p').textContent==='留在本轮的补充' && s.textContent.includes('补充未带入'));
    })())JS");
    evaluate("resume visible head","window.resumedMessage=document.querySelector('.queue-list li').dataset.message; document.querySelector('#dialogue-resume').click(); true");
    expect("resumed text appears in its own finished turn","!document.querySelector('.queue-list') && document.querySelectorAll('.turn').length===1 && "
        "document.querySelector('.turn').dataset.queuedMessage===window.resumedMessage && document.querySelector('.turn').dataset.exit==='succeeded' && "
        "document.querySelector('.turn .incoming p')?.textContent==='固定回复：稍后聊窗边的书'");
    evaluate("start a new original run for live input",R"JS((async()=>{
        const box=document.querySelector('#dialogue-draft'); box.value='等待桌面停止'; box.dispatchEvent(new Event('input'));
        await Promise.resolve(); document.querySelector('#dialogue-send').click(); return true;
    })())JS");
    wait_for("next composer run reached backend",[gate] { return gate->entered.load()==5; });
    expect("next turn resets the destination","document.querySelector('#dialogue-send').textContent==='追加本轮'");
    evaluate("double-click one current supplement",R"JS((async()=>{
        const box=document.querySelector('#dialogue-draft'); box.value='窗口补充，保持同一轮'; box.dispatchEvent(new Event('input'));
        await Promise.resolve(); const send=document.querySelector('#dialogue-send'); send.click(); send.click(); return true;
    })())JS");
    expect("one accepted supplement before model dispatch","document.querySelectorAll(':nth-last-child(1 of .turn) .turn-input').length===1 && "
        "document.querySelector(':nth-last-child(1 of .turn) .turn-input').dataset.state==='waiting' && !document.querySelector(':nth-last-child(1 of .turn) .incoming')");
    wait_for("release response to receive a same-turn supplement",[gate] { gate->release.release(); return true; });
    expect("one final answer from actual follow-up model request","document.querySelectorAll('.turn').length===2 && document.querySelector(':nth-last-child(1 of .turn)').dataset.exit==='succeeded' && "
        "document.querySelector(':nth-last-child(1 of .turn) .turn-input').dataset.state==='submitted' && document.querySelector(':nth-last-child(1 of .turn) .turn-input').dataset.step==='2' && "
        "document.querySelector(':nth-last-child(1 of .turn) .incoming p')?.textContent==='固定回复：窗口补充，保持同一轮'");
}
}
