import './style.css';
const view = new URL(location.href).searchParams.get('view');
const pending = new Map();
const output = document.querySelector('#result');
let sequence = 0;
// 发送固定探针消息，独立保留回执等待者，不触发宿主业务。
// 步骤：签发请求编号 → 登记回执等待 → 发送探针消息。
function request(method, identity = view, extra = {}, text = '你好，大肥鱼 🐟') {
  const id = String(++sequence);
  return new Promise(resolve => {
    pending.set(id, resolve);
    window.chrome.webview.postMessage({ request: id, view: identity, method, text, ...extra });
  });
}
// 依据宿主真实回执验证授权，不用页面本地判断替代原生校验。
// 步骤：核对来源隔离 → 正确回显 → 方法与代次越权 → 非法结构 → 改源触发撤销。
async function verify() {
  if (location.hash === '#offline' && navigator.onLine) throw new Error('浏览器未处于离线状态');
  let isolated = false;
  try { void parent.document.body; } catch { isolated = true; }
  if (!isolated) throw new Error('Frame 可以读取主页面，来源隔离失败');
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
// 组合结束和可信标记分别记录，协议模拟不能冒充系统输入法人工验收。
let compositions = 0;
let trustedEnds = 0;
document.querySelector('#frame-text').addEventListener('compositionend', event => {
  const status = document.querySelector('#composition');
  if (event.isTrusted) trustedEnds++;
  status.dataset.count = String(++compositions);
  status.textContent = '组合结束次数：' + compositions + '；可信标记：' + trustedEnds;
});
// 将用户明确提交的输入通过已授权回显链路返回，不触发其他业务。
// 步骤：取得当前输入并提交 → 等待原生裁定 → 显示结果文本。
document.querySelector('#echo').addEventListener('click', async () => {
  const reply = await request('probe.echo', view, {}, document.querySelector('#frame-text').value);
  output.textContent = reply.decision + '：' + reply.text;
});
// 在新文档中重放上一代身份，由原生宿主决定是否拒绝。
// 步骤：构造旧身份请求 → 等待原生裁定 → 显示拒绝种类。
document.querySelector('#stale').addEventListener('click', async () => {
  const reply = await request('probe.echo', String(BigInt(view) - 1n));
  output.textContent = reply.decision;
});
// 只有自动验收地址运行越权和导航探针，手动输入页保持授权可用。
if (location.hash === '#verify' || location.hash === '#offline') verify().catch(error => { output.textContent = error.message; });
