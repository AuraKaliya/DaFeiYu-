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
  const suspend = variant?.startsWith('partner-follow-collect');
  const follow = suspend || variant === 'partner-follow';
  const motion = follow || variant === 'partner-motion';
  const confirm = variant === 'partner-confirm';
  const scene = variant === 'partner-scene';
  const departure = variant === 'partner-departure';
  const revision = variant?.startsWith('partner-revision-');
  const notes = variant === 'partner-revision-notes';
  const correction = variant?.startsWith('partner-revision-correction');
  const correctionLost = variant === 'partner-revision-correction-lost';
  const repairObserve = variant === 'partner-revision-repair-observe';
  const repairPlan = variant === 'partner-revision-repair-plan';
  const questionExpiry = variant === 'partner-revision-question-expiry';
  const wall = [-60, -59].flatMap(y => [2, 3].map(x => ({ x, y, z: -5 })));
  const task = scene || variant === 'partner-task';
  const route = variant?.startsWith('partner-route-');
  const first = variant === 'partner-route-first';
  const delayed = variant === 'partner-route-delayed';
  const exchange = variant === 'partner-exchange';
  const showGame = confirm || (route && variant !== 'partner-route-panel' && variant !== 'partner-route-manual' && !delayed);
  const partnerLoop = variant === 'partner-loop' || interrupt || motion || confirm || task || route || exchange || departure || revision;
  fs.mkdirSync(temp, { recursive: true });
  const root = fs.mkdtempSync(path.join(temp, 'world-'));
  const config = path.join(root, 'user', 'plugins', 'mc.companion');
  fs.mkdirSync(config, { recursive: true });
  const channel = randomUUID();
  const key = randomBytes(32).toString('hex');
  fs.writeFileSync(path.join(config, 'settings.json'), JSON.stringify({ channel, bridgeKey: key }));
  let reads = 0;
  let baselineRead = false, pendingChat = false, replies = 0, stopped = 0, releases = 0, moveId, approvedId;
  let following, collecting, resumed, collectExited = false;
  let planReading = false, nextBlock = false;
  let wallFilled = false;
  let badObservations = 0;
  let questionPresented = false, expiredAt;
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
        const entered = { sequence: 1, type: 'player.entered' };
        const left = { sequence: 3, type: 'player.left' };
        const initial = !baselineRead;
        baselineRead = true;
        if (questionExpiry && questionPresented && expiredAt === undefined) {
          const trace = path.join(root, 'trace', 'events.jsonl');
          if (fs.existsSync(trace) && fs.readFileSync(trace, 'utf8').split('\n').some(line =>
            line.includes('"category":"mc.partner.question"') && line.includes('"status":"expired"'))) expiredAt = performance.now();
        }
        const latest = initial ? 1 : revision ? (planReading || (expiredAt !== undefined && performance.now() - expiredAt >= 700) ? 3 : 2) :
          departure ? (fs.existsSync(path.join(root, 'turn-started')) ? 3 : 2) :
          suspend ? resumed ? 5 : collecting ? 4 : following ? 3 : 2 :
          first && approvedId || motion && replies || confirm && pendingChat ? 4 :
          (interrupt || motion) && moveId || approvedId || delayed && fs.existsSync(path.join(root, 'new-instruction')) ? 3 : 2;
        const messages = [entered, { sequence: 2, text: 'play with me',
          sceneAtMessage: { dimension: 'overworld', sampledAtMs: 42, position: { x: -0.5, y: -57, z: 9.25 } } },
          { sequence: 3, text: motion || confirm ? 'where are you' : approvedId ? 'approve ' + approvedId : 'stop for now' },
          { sequence: 4, text: suspend ? 'can you hear me' : motion ? 'stop for now' : (confirm ? 'approve ' : 'deny ') + approvedId },
          { sequence: 5, text: 'stop following' }];
        if (departure) messages[2] = left;
        if (revision) {
          messages[1].text = '清理门边两格草方块';
          if (correction) {
            messages[1].text = '把我面前墙上的四格补上';
            messages[1].sceneAtMessage.position = { x: 3, y: -60, z: -7 };
          }
          messages[2].text = correction ? '我说的是墙，你应该竖着补' :
            notes ? '补记一下角落，接着做' : variant === 'partner-revision-goal' ? '改成清理另一处' :
            variant === 'partner-revision-scope' ? '只从地面取材' : '这次采掘先问我确认';
          if (questionExpiry) {
            messages[1].text = '把我面前的挖掉';
            messages[2].text = '你还在吗';
          }
        }
        return { generation, owner: 'fff01234-1234-1234-1234-123456789abc', oldest: 1, latest,
          gap: false, more: false, presence: departure && latest === 3 ? left : entered, messages: messages.slice(after, latest) };
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
      if (revision) {
        if (questionExpiry) {
          assert.equal(method, 'partner.begin');
          assert.equal(skill, 'reply');
          if (!questionPresented) {
            assert.equal(args.text, '你指左边还是右边？');
            assert.equal(context.order, 2);
            questionPresented = true;
          } else {
            assert.ok(expiredAt !== undefined);
            assert.equal(context.order, 3);
            assert.equal(args.text, '我在呢。');
            assert.equal(++replies, 1);
          }
          return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
        }
        if (repairObserve) {
          assert.equal(skill, 'observe');
          assert.equal(method, 'partner.begin');
          assert.deepEqual(args, { mode: 'scene', block: 'dirt' });
          assert.ok(++badObservations <= 2);
          return { ...context, skill, exited: true, result: { status: 'blocked', reasonCode: 'observe_mixed_modes' } };
        }
        assert.ok(!repairPlan);
        if (correction && skill === 'observe') {
          assert.ok(collectExited);
          assert.equal(args.message, 2);
          assert.deepEqual(args.verify, wall);
          return { ...context, skill, exited: true, result: { status: 'done', observation: {
            id, dimension: 'overworld', generation: context.generation, requested: 1, samples: [{
              sampledAtMs: 42, verified: wall.map((cell, i) => ({ ...cell, atSelf: true, block: i < 2 ? 'dirt' : 'air' })),
              views: [{ source: 'message', message: 2, status: 'sampled', playerFocus: { choice: '2,-60,-5:x',
                candidates: [{ id: '2,-60,-5:x', x: 2, y: -60, z: -5, width: 2, height: 2, axis: 'x' }] } },
              { source: 'self', status: 'sampled', actor: { position: { x: 3, y: -60, z: -6 } } }],
            }],
          } } };
        }
        if (correction && skill === 'fill_target') {
          if (method === 'partner.begin') {
            assert.ok(collectExited && !wallFilled);
            wallFilled = true;
            assert.deepEqual(args.cells, wall);
            assert.equal(args.sourceMessage, 2);
            assert.equal(args.taskRevision, 2);
            return { ...context, skill, exited: false, result: {} };
          }
          assert.equal(method, 'partner.read');
          return { ...context, skill, exited: true, result: { status: 'done', sourceMessage: 2, taskRevision: 2,
            placementAttempted: true, cells: wall.map((cell, i) => ({ ...cell, before: i < 2 ? 'dirt' : 'air',
              block: 'dirt', status: i < 2 ? 'present' : 'placed' })),
            changes: wall.slice(2).map(cell => ({ ...cell, before: 'air', after: 'dirt' })) } };
        }
        if (skill === 'reply' && variant === 'partner-revision-approval') {
          assert.match(args.text, /确认 \d+/);
          return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
        }
        assert.equal(skill, 'collect');
        const x = id === collecting || !collecting ? 1 : 2;
        if (method === 'partner.begin') {
          assert.deepEqual(args, { block: 'grass_block', x, y: -61, z: 2, count: 1, source: 'visible' });
          if (x === 1) { assert.equal(context.order, 2); collecting = id; }
          else { assert.ok(notes && collectExited && !nextBlock); assert.equal(context.order, 3); nextBlock = true; }
        }
        if (method === 'partner.read' && x === 1) planReading = true;
        if (method === 'partner.stop') {
          assert.ok(!notes && x === 1 && !nextBlock);
          ++stopped;
        }
        const exited = !correctionLost && (x === 2 || (notes ? fs.existsSync(path.join(root, 'revision-updated')) : stopped >= 2));
        if (!exited) return { ...context, skill, exited: false, result: {} };
        if (x === 1) collectExited = true;
        const status = notes || variant === 'partner-revision-goal' ? 'done' : 'interrupted';
        return { ...context, skill, exited: true, result: { status, broken: 1,
          changes: [{ x, y: -61, z: 2, before: 'grass_block', after: 'air' }], inventoryDelta: { dirt: 1 } } };
      }
      if (suspend) {
        const receipt = (exited, result) => ({ ...context, skill, exited, result });
        if (skill === 'follow') {
          assert.deepEqual(args ?? { distance: 4 }, { distance: 4 });
          if (method === 'partner.begin') {
            if (!following) following = id;
            else { assert.ok(collectExited); assert.equal(stopped, 1); resumed = id; }
          }
          assert.equal(id, resumed || following);
          if (method === 'partner.stop') ++stopped;
          return receipt(method === 'partner.stop', method === 'partner.stop' ? { status: 'interrupted' } : {});
        }
        if (skill === 'reply') {
          assert.ok(collecting && !collectExited && !resumed);
          assert.equal(args.text, 'I can hear you');
          ++replies;
          return receipt(true, { status: 'done', sent: true, text: args.text });
        }
        assert.equal(skill, 'collect');
        assert.equal(stopped, 1);
        if (method === 'partner.begin') {
          collecting = id;
          assert.deepEqual(args, { block: 'grass_block', x: 1, y: -61, z: 2, count: 1, source: 'visible' });
        }
        assert.equal(id, collecting);
        if (!replies) return receipt(false, {});
        collectExited = true;
        return receipt(true, variant.endsWith('-blocked') ? { status: 'blocked' } : { status: 'done', broken: 1,
          changes: [{ x: 1, y: -61, z: 2, before: 'grass_block', after: 'air' }], inventoryDelta: { dirt: 1 } });
      }
      if (showGame && skill === 'reply') {
        assert.equal(method, 'partner.begin');
        if (confirm && args.text === 'I can hear you while waiting.') {
          assert.ok(approvedId && !pendingChat && replies === 0);
          assert.deepEqual(context, { generation: state.generation, order: 3 });
          pendingChat = true;
          return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
        }
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
        assert.deepEqual(context, { generation: state.generation, order: confirm ? 4 : showGame ? 3 : 2 });
        assert.deepEqual(args, variant === 'partner-route-local' ?
          { count: 2, item: 'dirt', requiredStock: 2, source: 'ground' } : { count: 2, block: 'dirt' });
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
    return { generation, order: first || motion || confirm ? 4 :
      interrupt || showGame || delayed || departure || (revision && !repairObserve && !repairPlan) ? 3 : 2,
      phase: ++releases === 1 ? 'requested' : 'released' };
  },
  close() {} } : null);
  try {
    const child = spawn(exe, [packagePath, root, ...(interrupt || motion || confirm || task || route || exchange || departure || revision ? [variant] : [])],
      { stdio: 'inherit', windowsHide: true });
    const [code] = await once(child, 'exit');
    assert.equal(code, 0);
    if (partnerLoop) assert.equal(releases, 2);
    if (showGame) assert.ok(approvedId);
    if (questionExpiry) {
      assert.ok(questionPresented && expiredAt !== undefined);
      assert.equal(replies, 1);
      console.log(path.join(root, 'trace', 'events.jsonl'));
    } else if (repairObserve || repairPlan) {
      assert.equal(badObservations, repairObserve ? 2 : 0);
      console.log(path.join(root, 'trace', 'events.jsonl'));
    } else if (revision) {
      if (correctionLost) assert.ok(stopped >= 2);
      else assert.equal(stopped, notes ? 0 : 2);
      assert.equal(collectExited, !correctionLost);
      assert.equal(nextBlock, notes);
      assert.equal(wallFilled, !!correction && !correctionLost);
      console.log(path.join(root, 'trace', 'events.jsonl'));
    } else if (motion) { assert.equal(replies, 1); assert.equal(stopped, suspend ? 2 : 1); }
    else if (interrupt) assert.equal(stopped, 1);
    else if (partnerLoop) assert.equal(replies, first || delayed || departure ? 0 : 1);
    else if (boardWindow) assert.ok(reads > 0); else assert.equal(reads, 2);
  } finally {
    await bridge.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
