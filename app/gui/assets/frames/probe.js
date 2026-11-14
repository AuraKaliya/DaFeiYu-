const view = new URL(location.href).searchParams.get('view');
const pending = new Map();
const output = document.querySelector('#result');
let sequence = 0;
// 发送固定探针消息，独立保留回执等待者，不触发宿主业务。
function request(method, identity = view, extra = {}) {
  const id = String(++sequence);
  return new Promise(resolve => {
    pending.set(id, resolve);
    window.chrome.webview.postMessage({ request: id, view: identity, method, text: '你好，大肥鱼 🐟', ...extra });
  });
}
// 依据宿主真实回执验证授权，不用页面本地判断替代原生校验。
// 步骤：正确回显 → 方法与代次越权 → 非法结构 → 尝试改源触发撤销。
async function verify() {
  const echoed = await request('probe.echo');
  if (echoed.decision !== 'allowed' || echoed.text !== '你好，大肥鱼 🐟') throw new Error('中文回显失败');
  if ((await request('core.chat.submit')).decision !== 'forbidden_method') throw new Error('方法未拒绝');
  if ((await request('probe.echo', 'old-view')).decision !== 'stale_view') throw new Error('旧代次未拒绝');
  if ((await request('probe.echo', view, { forged: true })).decision !== 'malformed') throw new Error('非法字段未拒绝');
  output.textContent = '中文回显、方法与代次拒绝已通过；改源导航由原生宿主核对撤销。';
  location.href = 'https://center.dafeiyu.invalid/index.html';
}
// 只完成对应回执等待，导航后的授权状态以原生检查为准。
window.chrome.webview.addEventListener('message', event => {
  const resolve = pending.get(event.data.request);
  if (resolve) { pending.delete(event.data.request); resolve(event.data); }
});
// 验证异常只呈现在当前 Frame，不冒充宿主通过。
verify().catch(error => { output.textContent = error.message; });
