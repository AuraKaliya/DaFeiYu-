#include "../src/interaction_probe.hpp"
#include <fstream>

namespace dfy::ui {
// 两个正式窗口浏览同一封信，系统控制端单独推进取件与插件重载。
// 步骤：重复查看并保存原号 → 首窗退出 → 另一窗补读且首窗重新握手核实取件 → 复核来源换代。
void InteractionProbe::start_mailbox(std::wstring_view mode, const std::filesystem::path& data) {
    const auto marker = [&](const char* name) {
        wait_for(name,[file=data/name] { std::ofstream(file).put('1'); return true; });
    };
    const auto wait_marker = [&](const char* name) {
        wait_for(name,[file=data/name] { return std::filesystem::is_regular_file(file); });
    };
    expect("actual mailbox directory in the formal room","document.querySelector('.room-mailbox')?.dataset.directoryStatus==='ready' && "
        "[...document.querySelectorAll('.mail-source option')].some(o=>o.value.startsWith('plugin/test.mail/'))");
    evaluate("open the original plugin source",R"JS((()=>{
        document.querySelector('#room-mailbox-entry').click();
        const sel=document.querySelector('.mail-source select'), option=[...sel.options].find(o=>o.value.startsWith('plugin/test.mail/'));
        window.originalSource=option.value; window.mailSnapshots=0;
        window.watermarks=()=>nativeMail.mailboxes.map(box=>[box.id,box.direction,box.accepted,box.received]);
        window.chrome.webview.addEventListener('message',({data})=>{
            if(data.event==='mailbox.snapshot' && data.source===originalSource && data.state==='ready') {
                window.nativeMail=data; ++window.mailSnapshots;
            }
        });
        sel.value=option.value; sel.dispatchEvent(new Event('change')); return true;
    })())JS");
    if (mode==L"mailbox-reopen") {
        std::ifstream saved(data/"first-letter.json");
        Json original;
        saved >> original;
        evaluate("recover the first window's observed identities","window.firstVerified="+original.dump()+"; true");
        expect("a fresh native connection finds the original received letter",R"JS(mailSnapshots>0 &&
            nativeMail.instance===firstVerified[4] && originalSource===firstVerified[0] &&
            document.querySelector('[data-letter]')?.dataset.letter===firstVerified[1] &&
            document.querySelector('.mail-state')?.textContent==='原接收方已取件' && nativeMail.revision!==firstVerified[2] &&
            nativeMail.messages[0].receipt.acceptedRevision===firstVerified[5] &&
            nativeMail.messages[0].receipt.acceptedUtcMs===firstVerified[6])JS");
        evaluate("inspect the preserved acceptance time and the actual collection time",R"JS((()=>{
            const letter=document.querySelector('[data-letter]'); letter.querySelector('summary').click();
            const times=[...letter.querySelectorAll('time')].map(t=>Date.parse(t.dateTime));
            return letter.open && times[0]===firstVerified[6] && times[1]===nativeMail.messages[0].receipt.receivedUtcMs;
        })())JS");
        next();
        return;
    }
    const bool follow = mode==L"mailbox-follow";
    expect("original letter is still pending","mailSnapshots>0 && document.querySelectorAll('[data-letter]').length===1 && "
        "document.querySelector('[data-letter] summary')?.textContent.includes('test.notice') && document.querySelector('.mail-state')?.textContent==='待收取'");
    evaluate("read the public letter twice and reopen the mailbox",R"JS((()=>{
        const entry=document.querySelector('[data-letter]'), summary=entry.querySelector('summary'), box=document.querySelector('#room-mailbox-entry');
        window.originalLetter=entry.dataset.letter; window.originalRevision=document.querySelector('[data-mail-revision]').dataset.mailRevision;
        window.originalWatermarks=JSON.stringify(watermarks());
        summary.click(); summary.click(); summary.click(); box.click(); window.previousMailReads=mailSnapshots;
        return entry.open && !box.parentElement.open && entry.querySelector('time') &&
            watermarks().some(([,dir,accepted,received])=>dir==='toHost' && accepted==='1' && received==='0') &&
            watermarks().some(([,dir])=>dir==='toPlugin') && watermarks().every(([,dir,accepted])=>dir!=='toPlugin' || accepted==='0');
    })())JS");
    evaluate("return through the same room entry","document.querySelector('#room-mailbox-entry').click(); true");
    expect("reopening rereads the same message without consuming it","mailSnapshots>previousMailReads && document.querySelector('[data-letter]')?.dataset.letter===originalLetter && "
        "document.querySelector('[data-mail-revision]').dataset.mailRevision===originalRevision && JSON.stringify(watermarks())===originalWatermarks && "
        "document.querySelector('.mail-state').textContent==='待收取'");
    call("record the identities actually rendered by this window",L"Runtime.evaluate",
        {{"expression","[originalSource,originalLetter,originalRevision,originalWatermarks,nativeMail.instance,"
             "nativeMail.messages[0].receipt.acceptedRevision,nativeMail.messages[0].receipt.acceptedUtcMs]"},{"returnByValue",true}},
        [file=data/(follow ? "observer-letter.json" : "first-letter.json")](const Json& reply) {
            std::ofstream(file) << reply.at("result").at("value").dump();
        });
    if (!follow) { wait_marker("observer-letter.json"); next(); return; }

    wait_marker("first-closed");
    evaluate("interrupt this window's mailbox reads",R"JS((()=>{
        const bridge=window.chrome.webview, post=bridge.postMessage.bind(bridge);
        window.holdMailbox=true;
        Object.defineProperty(bridge,'postMessage',{configurable:true,value:fields=>{
            if(window.holdMailbox && fields.type.startsWith('mailbox.')) return;
            post(fields);
        }});
        return true;
    })())JS");
    evaluate("show last verified letter after loss of read acknowledgments",R"JS((async()=>{
        const end=performance.now()+8500;
        while(performance.now()<end) {
            const pane=document.querySelector('.room-mailbox');
            if(pane.dataset.directoryStatus==='unavailable' && pane.dataset.detailStatus==='unavailable')
                return pane.textContent.includes('上次读到') && pane.querySelector('[data-letter]')?.dataset.letter===originalLetter;
            await new Promise(done=>setTimeout(done,40));
        }
        return false;
    })())JS");
    marker("observer-offline");
    wait_marker("system-reviewed");
    evaluate("retry reading the original source","window.holdMailbox=false; document.querySelector('.mail-content button').click(); true");
    expect("system consumption updates the original letter","document.querySelector('.room-mailbox')?.dataset.detailStatus==='ready' && "
        "document.querySelector('[data-letter]')?.dataset.letter===originalLetter && document.querySelector('.mail-state').textContent==='原接收方已取件' && "
        "document.querySelector('[data-mail-revision]').dataset.mailRevision!==originalRevision && document.querySelector('[data-letter] dl').textContent.includes('取件时间') && "
        "watermarks().some(([,dir,accepted,received])=>dir==='toHost' && accepted==='1' && received==='1')");
    marker("received-visible");
    wait_marker("plugin-reloaded");
    expect("old selection is stale and replacement is independently available","document.querySelector('.room-mailbox')?.dataset.detailStatus==='stale' && "
        "!document.querySelector('[data-letter]') && [...document.querySelectorAll('.mail-source option')].some(o=>o.value.startsWith('plugin/test.mail/') && o.value!==originalSource)");
    evaluate("explicitly select the new plugin generation",R"JS((()=>{
        const sel=document.querySelector('.mail-source select');
        sel.value=[...sel.options].find(o=>o.value.startsWith('plugin/test.mail/') && o.value!==originalSource).value;
        sel.dispatchEvent(new Event('change')); return true;
    })())JS");
    expect("new source is confirmed empty","document.querySelector('.room-mailbox')?.dataset.detailStatus==='ready' && "
        "document.querySelector('.mail-feedback')?.textContent.includes('暂无信件') && !document.querySelector('[data-letter]')");
    next();
}
}
