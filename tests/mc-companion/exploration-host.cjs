const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { createInterface } = require('node:readline');
const { randomUUID, randomBytes } = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { openBridge } = require('../../plugins/mc/bridge/pipe.cjs');
const { returns } = require('./exploration.test.cjs');

// 用同一条移动轨迹驱动实际 DLL 与子 Session；夹具只决定看见的区域、采样断档和动作退出时刻。
// 步骤：发布固定游戏来源 → 交付一条新玩家委托 → 按原生验收信号推进轨迹 → 关闭管道。
async function run(exe, pkg, temp, mode) {
  fs.mkdirSync(temp, { recursive: true });
  const root = fs.mkdtempSync(path.join(temp, mode + '-'));
  const settings = path.join(root, 'user', 'plugins', 'mc.companion');
  fs.mkdirSync(settings, { recursive: true });
  const channel = randomUUID(), key = randomBytes(32).toString('hex');
  fs.writeFileSync(path.join(settings, 'settings.json'), JSON.stringify({ channel, bridgeKey: key }));
  const generation = randomUUID(), owner = randomUUID();
  const state = { generation, actor: randomUUID(), dimension: 'overworld', position: { x: 0, y: 0, z: 0 }, cobblestone: 0, signalAgeMs: 0 };
  let receive = 0, active = false, finished = false, offline = false, time = 360;
  let track = returns(mode === 'exploring');
  // 生成本次查看的真实协议形状，采集与返程用途均未授予。
  const action = () => ({ generation, order: 1, place: 'hillside', phase: finished ? 'arrived' : 'moving',
    exited: finished, moved: true, ...(finished ? { position: state.position, stone: null } : {}) });
  const bridge = await openBridge(channel, { read: () => offline ? null : state, close() {} }, {
    // 首次读取得到基线，之后仅产生一次绑定玩家的原话。
    read(_world, after) {
      const latest = receive++ ? 1 : 0;
      return { generation, owner, oldest: 1, latest, gap: false, more: false, messages: latest && !after ?
        [{ sequence: 1, text: '去山脚看看', offer: { purpose: 'visit.inspect', place: 'hillside' } }] : [] };
    }, close() {},
  }, key, {
    begin() { active = true; return action(); },
    poll(_world, _order, cancel) { if (cancel) finished = true; return action(); },
    progress() { return active && !finished ? track.read(time) : null; },
    explain(source, order, inquiry) {
      if (!active || finished || source !== generation || order !== 1) return null;
      return { order, inquiry, text: '东侧过路点尝试受阻，准备查看西侧。' };
    },
    close() {},
  });
  try {
    const child = spawn(exe, [pkg, root, mode], { stdio: ['ignore', 'pipe', 'inherit'], windowsHide: true });
    const lines = createInterface({ input: child.stdout });
    lines.on('line', signal => {
      if (signal === 'confirm') track.observe('0,0,0', 'old', ++time);
      if (signal === 'recover') track.observe('1,0,0', 'new-visible-wall', ++time);
      if (signal === 'new-window') {
        track.reset();
        for (let trip = 0; trip < 4; trip++) {
          track.observe('0,0,0', 'new-window', ++time);
          track.observe('1,0,0', 'other-' + trip, ++time);
        }
      }
      if (signal === 'disconnect') offline = true;
      if (signal === 'finish') finished = true;
    });
    const timer = setTimeout(() => child.kill(), 20000);
    try {
      const [code] = await once(child, 'close');
      if (code !== 0) throw new Error(mode + ' exploration review failed');
    } finally { clearTimeout(timer); lines.close(); }
  } finally { await bridge.close(); }
}
run(...process.argv.slice(2)).catch(error => { console.error(error); process.exitCode = 1; });
