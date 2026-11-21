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
  const interrupt = variant === 'partner-interrupt';
  const follow = variant === 'partner-follow';
  const motion = follow || variant === 'partner-motion';
  const confirm = variant === 'partner-confirm';
  const scene = variant === 'partner-scene';
  const task = scene || variant === 'partner-task';
  const route = variant?.startsWith('partner-route-');
  const first = variant === 'partner-route-first';
  const delayed = variant === 'partner-route-delayed';
  const exchange = variant === 'partner-exchange';
  const showGame = confirm || (route && variant !== 'partner-route-panel' && variant !== 'partner-route-manual' && !delayed);
  const partnerLoop = variant === 'partner-loop' || interrupt || motion || confirm || task || route || exchange;
  fs.mkdirSync(temp, { recursive: true });
  const root = fs.mkdtempSync(path.join(temp, 'world-'));
  const config = path.join(root, 'user', 'plugins', 'mc.companion');
  fs.mkdirSync(config, { recursive: true });
  const channel = randomUUID();
  const key = randomBytes(32).toString('hex');
  fs.writeFileSync(path.join(config, 'settings.json'), JSON.stringify({ channel, bridgeKey: key }));
  let reads = 0;
  let baselineRead = false, replies = 0, stopped = 0, releases = 0, moveId, approvedId;
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
        const latest = initial ? 1 : first && approvedId || motion && replies ? 4 :
          (interrupt || motion) && moveId || approvedId ? 3 : 2;
        const messages = [presence, { sequence: 2, text: 'play with me',
          sceneAtMessage: { dimension: 'overworld', sampledAtMs: 42, position: { x: -0.5, y: -57, z: 9.25 } } },
          { sequence: 3, text: motion ? 'where are you' : approvedId ? 'approve ' + approvedId : 'stop for now' },
          { sequence: 4, text: motion ? 'stop for now' : 'deny ' + approvedId }];
        return { generation, owner: 'fff01234-1234-1234-1234-123456789abc', oldest: 1, latest,
          gap: false, more: false, presence, messages: messages.slice(after, latest) };
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
    // 受控游戏场景只有在原任务完成登记后才产生新修订。
    scene(generation) {
      assert.equal(generation, state.generation);
      return scene ? (fs.existsSync(path.join(root, 'scene-changed')) ? 2 : 1) : null;
    },
    // 验证来自原回合的真实工具参数，并返回游戏实际发送的替身回执。
    call(method, id, context, skill, args) {
      if (showGame && skill === 'reply') {
        assert.equal(method, 'partner.begin');
        assert.deepEqual(context, { generation: state.generation, order: 2 });
        const match = /确认 (\d+) 或 拒绝 \1$/.exec(args.text);
        assert.ok(match && !approvedId);
        approvedId = match[1];
        return { ...context, skill, exited: true, result: { status: 'done', sent: true, reused: false, text: args.text } };
      }
      if (confirm || route) {
        assert.ok(!first && !delayed, 'a delivered approval must not restart a stale candidate');
        assert.equal(method, 'partner.begin');
        assert.equal(skill, 'collect');
        assert.deepEqual(context, { generation: state.generation, order: showGame ? 3 : 2 });
        assert.deepEqual(args, { count: 2, block: 'dirt' });
        ++replies;
        return { ...context, skill, exited: true, result: { status: 'done', collected: 2 } };
      }
      if (motion && skill === 'reply') {
        assert.equal(method, 'partner.begin');
        assert.equal(context.order, 3);
        assert.equal(args.text, 'still moving');
        assert.ok(moveId && stopped === 0);
        ++replies;
        return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
      }
      if (interrupt || motion) {
        assert.equal(skill, follow ? 'follow' : 'move');
        assert.deepEqual(context, { generation: state.generation, order: 2 });
        if (method === 'partner.begin') { moveId = id; assert.deepEqual(args, follow ? {} : { place: 'player' }); }
        else assert.equal(id, moveId);
        if (method === 'partner.stop') ++stopped;
        return { ...context, skill, exited: stopped > 0, result: stopped ? { status: 'interrupted' } :
          follow ? { progress: { phase: 'following', distance: 4.5 } } : {} };
      }
      assert.equal(method, 'partner.begin');
      assert.deepEqual(context, { generation: state.generation, order: 2 });
      assert.equal(skill, 'reply');
      assert.deepEqual(args, { text: exchange ? 'delegated hello' : 'ready to play' });
      ++replies;
      return { ...context, skill, exited: true, result: { status: 'done', sent: true, reused: false, text: args.text } };
    },
  },
  // 原世界释放第一次仍在途，第二次返回经过游戏核验的结束。
  releaseControl(generation) {
    assert.equal(generation, state.generation);
    return { generation, order: first || motion ? 4 : interrupt || showGame ? 3 : 2, phase: ++releases === 1 ? 'requested' : 'released' };
  },
  close() {} } : null);
  try {
    const child = spawn(exe, [packagePath, root, ...(interrupt || motion || confirm || task || route || exchange ? [variant] : [])], { stdio: 'inherit', windowsHide: true });
    const [code] = await once(child, 'exit');
    assert.equal(code, 0);
    if (partnerLoop) assert.equal(releases, 2);
    if (showGame) assert.ok(approvedId);
    if (motion) { assert.equal(replies, 1); assert.equal(stopped, 1); }
    else if (interrupt) assert.equal(stopped, 1);
    else if (partnerLoop) assert.equal(replies, first || delayed ? 0 : 1);
    else if (boardWindow) assert.ok(reads > 0); else assert.equal(reads, 2);
  } finally {
    await bridge.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
