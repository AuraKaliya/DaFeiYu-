#include "../app/gui/interaction_probe.hpp"
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
        return s.queuePaused && s.queue.length===8 && s.turns.at(-1).result==='cancelled' && s.turns.at(-1).exit==='pending';
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
}
}
