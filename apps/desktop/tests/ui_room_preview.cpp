#include "../src/interaction_probe.hpp"

namespace dfy::ui {
// 从主要预览入口验证公开样本，所有控制仅改变页面本地状态。
// 步骤：核对文本样本与首屏 → 检验键盘与消息去向 → 检验待决定事项和独立停止。
void InteractionProbe::start_room_preview(bool busy) {
    const auto sample = busy ? "busy" : "idle";
    expect("initial room", std::string("document.querySelector('main')?.dataset.sample === '") + sample +
        "' && document.querySelector('.sample-state dd')?.textContent === '" + (busy ? "书桌与窗边之间" : "书桌旁") + "'");
    expect("preview isolation", "document.querySelectorAll('iframe').length === 0 && !document.querySelector('#run-status')");
    call("focus emulation", L"Emulation.setFocusEmulationEnabled", {{"enabled",true}});
    expect("first content paint","performance.getEntriesByName('first-contentful-paint').length > 0");
    if (!busy) {
        evaluate("enter idle draft", "document.querySelector('textarea').focus(); document.activeElement.id === 'room-draft'");
        call("type into preview", L"Input.insertText", {{"text","你好，大肥鱼"}});
        expect("typed idle draft", "document.querySelector('textarea').value === '你好，大肥鱼'");
        key("send preview", "Enter", 13);
        expect("local send", "document.querySelector('#chat-body').textContent.includes('你好，大肥鱼') && !document.querySelector('textarea').value");
        evaluate("switch sample", "document.querySelector('#sample-busy').click(); true");
    }
    expect("busy destinations", "document.querySelector('.sample-lines').textContent.includes('待带入本轮') && document.querySelector('.sample-lines').textContent.includes('下一轮排队')");
    evaluate("open decision", "document.querySelector('summary').focus(); true");
    key("open with keyboard", "Enter", 13);
    expect("decision basis", "document.querySelector('details').open && document.querySelector('details p').textContent.includes('去窗边等候')");
    evaluate("answer and collapse", "document.querySelector('details button').click(); document.querySelector('#toggle-chat').click(); true");
    expect("collapsed chat", "!document.querySelector('#chat-body').getClientRects().length && document.querySelector('summary').textContent.includes('打开台灯')");
    call("narrow viewport", L"Emulation.setDeviceMetricsOverride", {{"width",360},{"height",690},{"deviceScaleFactor",1},{"mobile",false}});
    expect("narrow controls", "document.documentElement.scrollWidth <= innerWidth && ['#room-draft','#send-to','#stop-turn'].every(s => document.querySelector(s).getBoundingClientRect().right <= innerWidth)");
    evaluate("queue and send", "const sel=document.querySelector('#send-to'); sel.value='next'; sel.dispatchEvent(new Event('change')); document.querySelector('textarea').focus(); true");
    call("type queued", L"Input.insertText", {{"text","等会儿再聊"}});
    key("queue with enter", "Enter", 13);
    expect("queued locally", "document.querySelector('#chat-body').textContent.includes('下一轮排队等会儿再聊')");
    evaluate("stop current sample", "document.querySelector('#stop-turn').click(); true");
    expect("queue paused", "document.querySelector('#chat-body').textContent.includes('队列已暂停') && !document.querySelector('#stop-turn')");
    next();
}
}
