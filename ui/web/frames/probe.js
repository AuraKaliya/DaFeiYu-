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
// 真实组合输入与编码回显分别记录，脚本改值不计入输入法事件。
let compositions = 0;
document.querySelector('#frame-text').addEventListener('compositionend', event => {
  if (event.isTrusted) document.querySelector('#composition').textContent = '组合输入完成次数：' + (++compositions);
});
// 将用户明确提交的输入通过已授权回显链路返回，不触发其他业务。
// 步骤：取得当前输入并提交 → 等待原生裁定 → 显示结果文本。
document.querySelector('#echo').addEventListener('click', async () => {
  const reply = await request('probe.echo', view, {}, document.querySelector('#frame-text').value);
  output.textContent = reply.decision + '：' + reply.text;
});
// 只有自动验收地址运行越权和导航探针，手动输入页保持授权可用。
if (location.hash === '#verify') verify().catch(error => { output.textContent = error.message; });
