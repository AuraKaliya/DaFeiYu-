#include "../app/gui/interaction_probe.hpp"

namespace dfy::ui {
// 用真实面板制造两种刷新边界，在新页面按原编号验证唯一回合及草稿。
// 步骤：截住发送或后续读回 → 刷新主文档 → 核对原命令的实际回合和回复。
void InteractionProbe::check_dialogue_recovery() {
    call("count recovery lookups and original deliveries",L"Page.addScriptToEvaluateOnNewDocument",{{"source",R"JS(document.addEventListener('readystatechange',()=>{
        if (document.readyState!=='interactive' || !sessionStorage.getItem('fixture-submission')) return;
        const bridge=window.chrome.webview, post=bridge.postMessage.bind(bridge);
        window.recoveryTraffic=[];
        Object.defineProperty(bridge,'postMessage',{configurable:true,value:fields=>{
            if(fields.submission===sessionStorage.getItem('fixture-submission')) window.recoveryTraffic.push(fields.type);
            post(fields);
        }});
    }))JS"}});
    for (const bool delivered : {false,true}) {
        const auto prompt = delivered ? "已提交，回执待恢复" : "登记前刷新";
        const auto text = Json(prompt).dump();
        evaluate("hold one command or its receipt across reload",std::string("(async()=>{const delivered=")+(delivered?"true":"false")+",text="+text+R"JS(;
            const bridge=window.chrome.webview, post=bridge.postMessage.bind(bridge);
            let held=false;
            Object.defineProperty(bridge,'postMessage',{configurable:true,value:fields=>{
                if(fields.type==='dialogue.submit' && fields.text===text) {
                    held=true;
                    sessionStorage.setItem('fixture-original',fields.command);
                    sessionStorage.setItem('fixture-submission',fields.submission);
                    if(delivered) post(fields);
                    return;
                }
                if(!held || fields.type!=='dialogue.read') post(fields);
            }});
            const box=document.querySelector('#dialogue-draft');
            box.value=text; box.dispatchEvent(new Event('input')); await Promise.resolve();
            document.querySelector('#dialogue-send').click();
            if(delivered) { box.value='另一个草稿'; box.dispatchEvent(new Event('input')); }
            const original=history.state.dialogue.commands[0];
            return held && original.command===sessionStorage.getItem('fixture-original') && original.submission?.length===36 &&
                original.submission===sessionStorage.getItem('fixture-submission') && Number.isSafeInteger(original.sourceUtcMs);
        })())JS");
        call("reload pending main document",L"Page.reload",Json::object());
        expect("only one restored command reaches its own final turn",
            "location.origin==='https://panel.dafeiyu.invalid' && history.state?.dialogue?.commands.length===0 && "
            "[...document.querySelectorAll('.outgoing p')].filter(p=>p.textContent==="+text+").length===1 && "
            "document.querySelector(':nth-last-child(1 of .turn) .incoming p')?.textContent==="+Json(std::string("固定回复：")+prompt).dump()+
            " && document.querySelector('#dialogue-draft').value==="+Json(delivered?"另一个草稿":"").dump());
        evaluate("check stored receipt before replaying only a missing submission",
            "window.recoveryTraffic?.[0]==='messages.receipt' && recoveryTraffic.filter(t=>t==='dialogue.submit').length==="+std::to_string(delivered ? 0 : 1));
        evaluate("recovered receipt matches original command",R"JS((async()=>{
            const bridge=window.chrome.webview, id=sessionStorage.getItem('fixture-original');
            const found=new Promise(resolve=>{
                const receive=({data})=>{
                    if(data.request!=='recovery-original') return;
                    bridge.removeEventListener('message',receive); resolve(data.receipt);
                };
                bridge.addEventListener('message',receive);
            });
            bridge.postMessage({type:'dialogue.read',version:'1',request:'recovery-original',command:id,revision:''});
            const receipt=await Promise.race([found,new Promise(resolve=>setTimeout(()=>resolve(null),1200))]);
            return receipt?.command===id && receipt.state==='accepted' && receipt.turn===document.querySelector(':nth-last-child(1 of .turn)').dataset.turn;
        })())JS");
    }
}
}
