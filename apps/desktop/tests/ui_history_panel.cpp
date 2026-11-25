#include "../src/interaction_probe.hpp"
#include <fstream>

namespace dfy::ui {
// 多个真实窗口共用一个独立的固定响应后台，测试信号只用于控制关闭和断连的边界。
// 步骤：发送窗提交长对话 → 观察窗从原游标追赶 → 新窗口翻到首条并刷新 → 重启后复查原号。
void InteractionProbe::start_history(std::wstring_view mode, const std::filesystem::path& data) {
    const auto marker = [&](const char* file) {
        wait_for(file,[path=data/file] { std::ofstream(path).put('1'); return true; });
    };
    const auto await_marker = [&](const char* file) {
        wait_for(file,[path=data/file] { return std::filesystem::is_regular_file(path); });
    };
    const std::string ready = "document.querySelector('#dialogue-state')?.dataset.accepting==='true'";
    const std::string last = "document.querySelector(':nth-last-child(1 of .turn)')";
    const std::string all = R"JS((()=>{
        const rows=[...document.querySelectorAll('[data-stored-message]')], texts=rows.map(r=>r.querySelector('p')?.textContent);
        return rows.length===38 && new Set(rows.map(r=>r.dataset.storedMessage)).size===38 && rows.every((r,i)=>
            Number.isFinite(Date.parse(r.querySelector('time')?.dateTime)) && (!i || BigInt(r.dataset.storedMessage)>BigInt(rows[i-1].dataset.storedMessage))) &&
            texts[0]==='历史窗口 1' && texts[1]==='固定回复：历史窗口 1' && texts.at(-1)==='固定回复：两个窗口关闭后再回答' &&
            texts.filter(t=>t==='两次独立的同一句').length===2 && texts.filter(t=>t==='固定回复：两次独立的同一句').length===2;
    })())JS";
    expect("formal composer is connected",ready);
    if (mode==L"history-send") {
        await_marker("watch-ready");
        evaluate("bind fixture to the formal send button",R"JS((()=>{
            window.sendFixture=async text=>{
                window.beforeTurn=document.querySelector(':nth-last-child(1 of .turn)')?.dataset.turn;
                const draft=document.querySelector('#dialogue-draft'), button=document.querySelector('#dialogue-send');
                draft.value=text; draft.dispatchEvent(new Event('input')); await Promise.resolve();
                if(button.disabled) return false;
                button.click(); button.click(); return true;
            };
            return true;
        })())JS");
        for (int i=1; i<=18; ++i) {
            const auto prompt = i>16 ? "两次独立的同一句" : "历史窗口 "+std::to_string(i);
            evaluate("send round "+std::to_string(i),"sendFixture("+Json(prompt).dump()+")");
            expect("stored answer "+std::to_string(i),ready+" && "+last+"?.dataset.turn!==window.beforeTurn && "+
                last+"?.querySelector('.incoming[data-stored-message] p')?.textContent==="+Json("固定回复："+prompt).dump());
            if (i==1) { marker("first-stored"); await_marker("watch-disconnected"); }
        }
        evaluate("submit the delayed question","sendFixture('两个窗口关闭后再回答')");
        await_marker("reply-held");
        await_marker("watch-caught-up");
    } else if (mode==L"history-watch") {
        marker("watch-ready");
        await_marker("first-stored");
        expect("second window independently sees the first pair","document.querySelectorAll('[data-stored-message]').length===2");
        evaluate("hold the observer connection until the UI detects loss",R"JS((async()=>{
            const bridge=window.chrome.webview, post=bridge.postMessage.bind(bridge);
            window.holdReads=true; window.catchup=[];
            Object.defineProperty(bridge,'postMessage',{configurable:true,value:fields=>{
                if(window.holdReads && fields.type==='dialogue.read') return;
                if(!window.holdReads && (fields.type==='messages.subscribe' || fields.direction==='changes')) window.catchup.push(fields);
                post(fields);
            }});
            const end=performance.now()+6500;
            while (document.querySelector('#dialogue-state').dataset.ready==='true' && performance.now()<end)
                await new Promise(done=>setTimeout(done,40));
            return document.querySelector('#dialogue-state').dataset.ready==='false' && document.querySelectorAll('[data-stored-message]').length===2;
        })())JS");
        marker("watch-disconnected");
        await_marker("reply-held");
        evaluate("resume the original observer","window.holdReads=false; true");
        expect("second window catches every missed row from its prior cursor",R"JS((()=>{
            const rows=[...document.querySelectorAll('[data-stored-message]')];
            return rows.length===37 && new Set(rows.map(r=>r.dataset.storedMessage)).size===37 &&
                rows.filter(r=>r.querySelector('p').textContent==='两次独立的同一句').length===2 &&
                rows.at(-1).querySelector('p').textContent==='两个窗口关闭后再回答' &&
                catchup[0]?.type==='messages.subscribe' && catchup.some(r=>r.direction==='changes' && BigInt(r.cursor)>0n);
        })())JS");
        marker("watch-caught-up");
    } else if (mode==L"history-reopen") {
        expect("newest page includes the closed-window answer","document.querySelector('#history-earlier') && "+
            last+"?.querySelector('.incoming[data-stored-message] p')?.textContent==='固定回复：两个窗口关闭后再回答' && "+
            "![...document.querySelectorAll('.outgoing p')].some(p=>p.textContent==='历史窗口 1')");
        evaluate("page back from the real transcript","document.querySelector('#history-earlier').click(); true");
        expect("first page and all committed pairs",all);
        evaluate("read an older message partway down the first page",R"JS((()=>{
            const list=document.querySelector('.transcript'), top=list.getBoundingClientRect().top;
            const row=list.querySelectorAll('[data-stored-message]')[5];
            list.scrollTop+=row.getBoundingClientRect().top-top+8;
            const first=[...list.querySelectorAll('[data-stored-message]')].find(el=>el.getBoundingClientRect().bottom>top);
            sessionStorage.setItem('fixture-position',JSON.stringify([first.dataset.storedMessage,first.getBoundingClientRect().top-top]));
            window.fixtureBeforeRefresh=true; return first===row;
        })())JS");
        call("refresh the same window after reading the oldest page",L"Page.reload",Json::object());
        expect("refresh restores the loaded range","!window.fixtureBeforeRefresh && "+all);
        expect("refresh restores the same visible message and pixel offset",R"JS((()=>{
            const [message,offset]=JSON.parse(sessionStorage.getItem('fixture-position')), list=document.querySelector('.transcript');
            const row=list.querySelector('[data-stored-message="'+message+'"]');
            return !!row && Math.abs(row.getBoundingClientRect().top-list.getBoundingClientRect().top-offset)<2;
        })())JS");
    } else {
        expect("uncertain prior delivery retains its original text","document.querySelector('[data-receipt=unknown] button') && "
            "document.querySelector('[data-receipt=unknown]').parentElement.querySelector('p').textContent==='重启前待核对的原话'");
        evaluate("capture only the user-requested receipt check",R"JS((()=>{
            const bridge=window.chrome.webview, post=bridge.postMessage.bind(bridge);
            window.rechecked=[]; window.writes=0;
            Object.defineProperty(bridge,'postMessage',{configurable:true,value:fields=>{
                if(fields.type==='messages.receipt') window.rechecked.push(fields);
                if(fields.type==='dialogue.submit') ++window.writes;
                post(fields);
            }});
            document.querySelector('[data-receipt=unknown] button').click(); return true;
        })())JS");
        expect("unknown is rechecked under the original submission without resending",R"JS(rechecked.length===1 &&
            rechecked[0].source==='desktop' && rechecked[0].submission==='interrupted-original' && writes===0 &&
            document.querySelector('[data-receipt=unknown]')?.textContent.includes('已核对，结果仍未知'))JS");
    }
    next();
}
}
