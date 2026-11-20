const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { randomUUID, randomBytes } = require('node:crypto');
const { once } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const { openBridge } = require('../../plugins/mc/bridge/pipe.cjs');

// 用独立目录的插件配置把真实 DLL 接到受控世界来源。
// 步骤：签发管道短名并保存用户设置 → 运行原生安装与执行探针 → 核对调用数并关闭。
async function main() {
  const [exe, packagePath, temp, variant] = process.argv.slice(2);
  const boardWindow = variant === 'board-window';
  const partnerLoop = variant === 'partner-loop';
  fs.mkdirSync(temp, { recursive: true });
  const root = fs.mkdtempSync(path.join(temp, 'world-'));
  const config = path.join(root, 'user', 'plugins', 'mc.companion');
  fs.mkdirSync(config, { recursive: true });
  const channel = randomUUID();
  const key = randomBytes(32).toString('hex');
  fs.writeFileSync(path.join(config, 'settings.json'), JSON.stringify({ channel, bridgeKey: key }));
  let reads = 0;
  let baselineRead = false, replies = 0;
  const state = { generation: randomUUID(), actor: randomUUID(), dimension: 'overworld',
    position: { x: -1.5, y: -60, z: 0.5 }, cobblestone: 5, signalAgeMs: 20 };
  const bridge = await openBridge(channel, {
    // 第二次查询模拟原世界已撤销。
    read() { ++reads; return partnerLoop || boardWindow || reads === 1 ? state : null; },
    // 替身没有待释放的游戏资源。
    close() {},
  }, {
    // 返回特定来源的固定玩家原话，供私有收件读口验收。
    read(generation, after) {
      if (generation !== state.generation || (!partnerLoop && !boardWindow && reads !== 1)) return null;
      if (partnerLoop) {
        const presence = { sequence: 1, type: 'player.entered' };
        const initial = !baselineRead;
        baselineRead = true;
        return { generation, owner: 'fff01234-1234-1234-1234-123456789abc', oldest: 1, latest: initial ? 1 : 2,
          gap: false, more: false, presence, messages: initial ? [presence] : after === 1 ? [{ sequence: 2, text: 'play with me' }] : [] };
      }
      if (boardWindow) {
        const latest = baselineRead ? 40 : 0;
        baselineRead = true;
        const end = Math.min(latest, after + 8);
        const messages = [];
        for (let sequence = after + 1; sequence <= end; ++sequence)
          messages.push({ sequence, text: '固定查看 ' + sequence, offer: { purpose: 'visit.inspect', place: 'hillside' } });
        return { generation, owner: 'fff01234-1234-1234-1234-123456789abc', oldest: 1, latest, gap: false, more: end < latest, messages };
      }
      return { generation, owner: 'fff01234-1234-1234-1234-123456789abc', oldest: 1, latest: 1,
        gap: false, more: false, messages: after === 0 ? [{ sequence: 1, text: '去山脚看看' }] : [] };
    },
    // 受控收件源不保留外部连接。
    close() {},
  }, key, partnerLoop ? { partner: {
    // 验证来自原回合的真实工具参数，并返回游戏实际发送的替身回执。
    call(method, id, context, skill, args) {
      assert.equal(method, 'partner.begin');
      assert.deepEqual(context, { generation: state.generation, order: 2 });
      assert.equal(skill, 'reply');
      assert.deepEqual(args, { text: 'ready to play' });
      ++replies;
      return { ...context, skill, exited: true, result: { status: 'done', sent: true, reused: false, text: args.text } };
    },
  }, close() {} } : null);
  try {
    const child = spawn(exe, [packagePath, root], { stdio: 'inherit', windowsHide: true });
    const [code] = await once(child, 'exit');
    assert.equal(code, 0);
    if (partnerLoop) assert.equal(replies, 1);
    else if (boardWindow) assert.ok(reads > 0); else assert.equal(reads, 2);
  } finally {
    await bridge.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
