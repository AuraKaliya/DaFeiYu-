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
  const recovery = variant === 'partner-follow-recovery';
  const follow = suspend || recovery || variant === 'partner-follow';
  const motion = follow || variant === 'partner-motion';
  const confirm = variant === 'partner-confirm';
  const scene = variant === 'partner-scene';
  const departure = variant === 'partner-departure';
  const revision = variant?.startsWith('partner-revision-');
  const directRevision = variant?.startsWith('partner-revision-direct-');
  const directPlace = variant === 'partner-revision-direct-place';
  const notes = revision && variant.endsWith('-notes');
  const correction = variant?.startsWith('partner-revision-correction');
  const correctionLost = variant === 'partner-revision-correction-lost';
  const navigation = variant?.startsWith('partner-revision-navigation');
  const restartRoute = variant === 'partner-revision-navigation-restart';
  const mixedRoute = variant === 'partner-revision-navigation-mixed';
  const repairObserve = variant === 'partner-revision-repair-observe';
  const repairPlan = variant === 'partner-revision-repair-plan';
  const layered = variant?.startsWith('partner-revision-layered');
  const layeredBlocked = variant === 'partner-revision-layered-blocked';
  const attention = variant === 'partner-revision-attention';
  const questionExpiry = variant === 'partner-revision-question-expiry';
  const pointedSupport = variant === 'partner-revision-pointed-support';
  const pointedCraft = variant?.startsWith('partner-revision-pointed-craft');
  const noPickup = variant === 'partner-revision-pointed-craft-no-pickup';
  const wall = [-60, -59].flatMap(y => [2, 3].map(x => ({ x, y, z: -5 })));
  const pads = [{ x: 3, y: -62, z: 24 }, { x: 3, y: -61, z: 25 }];
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
  let blockedReads = 0, recoveredReads = 0;
  let attentionSamples = 0;
  let planReading = false, nextBlock = false;
  const nodeExits = [];
  let nodeAtWork;
  let wallFilled = false;
  let badObservations = 0;
  let questionPresented = false, expiredAt;
  let clarificationAck = false;
  let heardAt;
  let surveyId, checkedRoute, routeWork, routeReads = 0, oldCheckpoint = -1;
  let restartWork, restartReads = 0, routeStops = 0, followIsRunning = false;
  let routeApproach;
  const waypoints = [[1, -6], [4, -6], [4, -4], [1, -4]].map(([x, z]) => ({ x, y: -60, z }));
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
        const latest = initial ? 1 :
          restartRoute ? (oldCheckpoint >= 5 ? 5 : oldCheckpoint >= 2 ? 4 : followIsRunning ? 3 : 2) :
          pointedCraft ? (heardAt !== undefined && performance.now() - heardAt > 300 ? 4 : questionPresented ? 3 : 2) :
          attention ? (replies === 2 ? 4 : recoveredReads >= 8 ? 3 : 2) :
          revision ? (planReading || (expiredAt !== undefined && performance.now() - expiredAt >= 700) ? 3 : 2) :
          departure ? (fs.existsSync(path.join(root, 'turn-started')) ? 3 : 2) :
          suspend ? resumed ? 5 : collecting ? 4 : following ? 3 : 2 :
          recovery ? recoveredReads > 1 ? 4 : blockedReads > 1 ? 3 : 2 :
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
          if (attention) {
            messages[1].text = '大肥鱼，跟着我';
            messages[2].text = '你听得到吗';
            messages[3].text = '不用跟了';
          }
          if (layered) {
            messages[1].text = '你可以垫上来';
            messages[1].sceneAtMessage.position = { x: 3.5, y: -60, z: 25.5 };
          }
          if (correction || navigation || directPlace) {
            messages[1].text = '把我面前墙上的四格补上';
            messages[1].sceneAtMessage.position = { x: 3, y: -60, z: -7 };
          }
          messages[2].text = navigation ? '现在绕着这个墙跑两圈' : correction ? '我说的是墙，你应该竖着补' :
            notes ? '补记一下角落，接着做' : variant === 'partner-revision-goal' || directPlace ? '改成清理另一处' :
            variant.endsWith('-scope') ? '只从地面取材' : '这次采掘先问我确认';
          if (questionExpiry) {
            messages[1].text = '把我面前的挖掉';
            messages[2].text = '你还在吗';
          }
          if (pointedSupport) messages[1].text = '把这石块上的泥土敲掉';
          if (pointedCraft) {
            messages[1].text = '这里有个原木，敲掉它，然后做成4个木板';
            messages[2].text = '你听得到吗';
            messages[3].text = '就是这个';
          }
          if (restartRoute) {
            messages[1].text = '大肥鱼，跟着我走';
            messages[3].text = '要不要计时';
            messages[4].text = '中途重新绕';
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
        if (attention) {
          if (skill === 'reply') {
            assert.equal(method, 'partner.begin');
            assert.equal(context.order, replies === 0 ? 2 : 3);
            assert.equal(args.text, replies === 0 ? '我被高台挡住了，你等我找一下路。' : '听得见，我跟上了。');
            assert.ok(++replies <= 2);
            return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
          }
          assert.equal(context.order, 2);
          assert.equal(skill, 'follow');
          if (method === 'partner.begin') { assert.ok(!following); following = id; }
          assert.equal(id, following);
          if (method === 'partner.stop') {
            ++stopped;
            return { ...context, skill, exited: true, result: { status: 'interrupted' } };
          }
          const sampledAtMs = ++attentionSamples * 500;
          if (replies) ++recoveredReads;
          const progress = recoveredReads > 4 ? { phase: 'in_range', terrain: 'dry', distance: 1.5, vertical: 0, path: 'reachable' } :
            attentionSamples <= 3 ? { phase: 'following', terrain: 'swimming', distance: 4, vertical: 0.5, path: 'reachable' } :
              { phase: replies ? 'blocked' : 'following', terrain: 'dry', distance: 8, vertical: 5, path: 'unreachable' };
          return { ...context, skill, exited: false, result: { progress: { ...progress, sampledAtMs } } };
        }
        if (layered) {
          assert.equal(context.order, 2);
          if (layeredBlocked && skill === 'reply') {
            assert.equal(method, 'partner.begin');
            assert.equal(args.text, '第一块垫好了，但我站不上去。');
            assert.equal(nodeExits.length, 2);
            assert.equal(++replies, 1);
            return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
          }
          if (skill === 'observe') {
            assert.equal(method, 'partner.begin');
            assert.deepEqual(args, { sources: ['self'], verify: pads });
            return { ...context, skill, exited: true, result: { status: 'done', observation: {
              id, generation: context.generation, dimension: 'overworld', requested: 1, samples: [{
                sampledAtMs: 42, verified: pads.map(cell => ({ ...cell, atSelf: true, block: 'air' })),
                views: [{ source: 'self', status: 'sampled', actor: { position: { x: 3.5, y: -62, z: 23.5 } } }],
              }],
            } } };
          }
          const index = nodeExits.length;
          const pad = pads[Math.floor(index / 2)];
          const place = index % 2 === 0;
          assert.ok(index < (layeredBlocked ? 2 : 4));
          assert.equal(skill, place ? 'place' : 'move');
          if (method === 'partner.begin') {
            assert.ok(!nodeAtWork && !nodeExits.includes(id));
            assert.deepEqual(args, place ? { ...pad, block: 'dirt', sourceMessage: 2, taskRevision: 2 } :
              { ...pad, y: pad.y + 1, exact: true });
            nodeAtWork = id;
            return { ...context, skill, exited: false, result: {} };
          }
          assert.equal(method, 'partner.read');
          assert.equal(id, nodeAtWork);
          nodeExits.push(id);
          nodeAtWork = undefined;
          const result = place ? { status: 'done', placementAttempted: true, placed: true,
            sourceMessage: 2, taskRevision: 2, blocks: [{ ...pad, block: 'dirt' }],
            changes: [{ ...pad, before: 'air', after: 'dirt' }], materialBefore: 2 - index / 2, materialAfter: 1 - index / 2 } :
            layeredBlocked ? { status: 'blocked', reasonCode: 'unsafe_stand' } :
              { status: 'done', arrived: true, position: { x: pad.x + 0.5, y: pad.y + 1, z: pad.z + 0.5 } };
          return { ...context, skill, exited: true, result };
        }
        if (questionExpiry) {
          assert.equal(method, 'partner.begin');
          assert.equal(skill, 'reply');
          if (!clarificationAck) {
            assert.equal(context.order, 2);
            assert.equal(args.text, '我来看看。');
            clarificationAck = true;
          } else if (!questionPresented) {
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
        if (repairPlan) {
          assert.equal(method, 'partner.begin');
          assert.equal(skill, 'reply');
          assert.equal(context.order, 2);
          assert.equal(args.text, '这次没动起来，我得换个办法。');
          assert.equal(++replies, 1);
          return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
        }
        if (pointedCraft) {
          const log = { x: 1, y: -59, z: 2, block: 'oak_log' };
          if (skill === 'observe') {
            assert.equal(method, 'partner.begin');
            assert.ok([2, 4].includes(context.order));
            assert.deepEqual(args, { mode: 'pointed', message: context.order, focus: 'objects' });
            const objects = [{ ...log, visible: true }];
            if (context.order === 2) objects.push({ ...log, x: 2, visible: true });
            return { ...context, skill, exited: true, result: { status: 'done', anchor: { message: context.order },
              sampledAtMs: 42, playerFocus: { status: 'observed', hit: null, objects } } };
          }
          if (skill === 'reply') {
            assert.equal(method, 'partner.begin');
            switch (++replies) {
              case 1: assert.equal(args.text, '你指哪块原木？'); questionPresented = true; break;
              case 2: assert.equal(args.text, '听得到。'); heardAt = performance.now(); break;
              case 3:
                assert.ok(collectExited);
                assert.equal(args.text, noPickup ? '木头敲下来了，我还没捡到。' : '木头捡到了，我现在做不了木板。');
                break;
              default: assert.fail('unexpected repeated game reply');
            }
            return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
          }
          assert.equal(skill, 'collect');
          assert.equal(context.order, 4);
          if (method === 'partner.begin') {
            assert.ok(!collecting);
            assert.deepEqual(args, { ...log, count: 1, source: 'visible' });
            collecting = id;
            return { ...context, skill, exited: false, result: {} };
          }
          assert.equal(method, 'partner.read');
          assert.equal(id, collecting);
          collectExited = true;
          const { block, ...position } = log;
          return { ...context, skill, exited: true, result: { status: 'done', broken: 1,
            changes: [{ ...position, before: block, after: 'air' }], inventoryDelta: noPickup ? {} : { oak_log: 1 } } };
        }
        if (pointedSupport) {
          const stone = { x: 1, y: -60, z: 2, block: 'stone' };
          const dirt = { x: 1, y: -59, z: 2, block: 'dirt' };
          assert.equal(context.order, 2);
          if (skill === 'observe') {
            assert.equal(method, 'partner.begin');
            assert.deepEqual(args, { mode: 'pointed', message: 2, focus: 'objects' });
            return { ...context, skill, exited: true, result: { status: 'done', anchor: { message: 2 }, sampledAtMs: 42,
              playerFocus: { status: 'observed', hit: stone, objects: [{ ...dirt, visible: true, below: 'stone' }] } } };
          }
          assert.equal(skill, 'collect');
          if (method === 'partner.begin') {
            assert.ok(!collecting);
            assert.deepEqual(args, { ...dirt, count: 1, source: 'visible', digReference: stone });
            collecting = id;
            return { ...context, skill, exited: false, result: {} };
          }
          assert.equal(method, 'partner.read');
          assert.equal(id, collecting);
          collectExited = true;
          const { block, ...position } = dirt;
          return { ...context, skill, exited: true, result: { status: 'done', broken: 1,
            changes: [{ ...position, before: block, after: 'air' }], inventoryDelta: { dirt: 1 } } };
        }
        if (restartRoute && skill === 'follow') {
          if (method === 'partner.begin') {
            if (!following) following = id;
            else {
              assert.equal(restartReads, 9);
              assert.equal(id, following + '.resume.2');
              resumed = id;
              fs.writeFileSync(path.join(root, 'original-follow-resumed'), '');
            }
          }
          assert.equal(id, resumed || following);
          if (method === 'partner.read') followIsRunning = true;
          return { ...context, skill, exited: method === 'partner.stop',
            result: method === 'partner.stop' ? { status: 'interrupted' } : {} };
        }
        if (restartRoute && skill === 'reply') {
          assert.ok(routeWork && !restartWork && !questionPresented);
          assert.equal(context.order, 4);
          assert.equal(args.text, '要从这圈开始计时吗？');
          questionPresented = true;
          return { ...context, skill, exited: true, result: { status: 'done', sent: true } };
        }
        if (navigation && skill === 'observe') {
          assert.equal(method, 'partner.begin');
          assert.ok(restartRoute || collectExited);
          if (args.mode === 'wall') {
            assert.deepEqual(args, { mode: 'wall', message: 3 });
            surveyId = id;
            return { ...context, skill, exited: true, result: { status: 'done',
              wallSurvey: { id, status: 'selected', choice: 'wall' } } };
          }
          assert.deepEqual(args, { mode: 'route', observation: surveyId, ...(context.order === 5 ? { wall: 'wall' } : {}) });
          checkedRoute = id;
          return { ...context, skill, exited: true, result: { status: 'done', route: { id, status: 'validated',
            generation: context.generation, dimension: 'overworld', sourceMessage: 3, observation: surveyId, wall: 'wall', waypoints } } };
        }
        if (navigation && skill === 'route') {
          if (method === 'partner.begin') {
            assert.ok(!resumed);
            if (mixedRoute) assert.ok(routeApproach);
            if (routeWork) {
              assert.ok(restartRoute && routeStops === 1 && !restartWork);
              restartWork = id;
            } else routeWork = id;
            assert.deepEqual(args, { route: checkedRoute, taskId: 3, taskRevision: restartRoute && !restartWork ? 1 : 2, waypoints, laps: 2 });
          }
          assert.equal(id, restartWork || routeWork);
          if (method === 'partner.stop') ++routeStops;
          const checkpoint = restartWork ? restartReads++ : Math.min(routeReads++, restartRoute ? questionPresented ? 5 : 2 : 8);
          if (!restartWork) oldCheckpoint = checkpoint;
          const checkpoints = Array.from({ length: checkpoint + 1 }, (_, index) =>
            ({ index, position: { ...waypoints[index % 4], x: waypoints[index % 4].x + 0.5 } }));
          const laps = Math.floor(checkpoint / 4);
          const status = method === 'partner.stop' ? 'interrupted' : laps === 2 ? 'done' : null;
          return { ...context, skill, exited: Boolean(status), result: {
            ...(status && { status }),
            progress: { phase: 'route', checkpoint, laps, requested: 2 },
            route: { id: checkedRoute, requested: 2, laps, checkpoints },
          } };
        }
        if (mixedRoute && skill === 'move') {
          assert.equal(method, 'partner.begin');
          assert.ok(collectExited && checkedRoute && !routeApproach && !routeWork);
          assert.deepEqual(args, { ...waypoints[0], exact: true });
          routeApproach = id;
          return { ...context, skill, exited: true, result: { status: 'done', arrived: true,
            position: { ...waypoints[0], x: waypoints[0].x + 0.5 } } };
        }
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
        if (skill === 'reply' && variant.endsWith('-approval')) {
          assert.match(args.text, /确认 \d+/);
          return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
        }
        if (directRevision) {
          assert.equal(skill, directPlace ? 'place' : 'collect');
          assert.equal(context.order, 2);
          if (method === 'partner.begin') {
            assert.equal(collecting, undefined);
            assert.deepEqual(args, directPlace ? { ...wall[0], block: 'dirt', sourceMessage: 2, taskRevision: 1 } :
              { block: 'grass_block', count: 2 });
            collecting = id;
          }
          assert.equal(id, collecting);
          if (method === 'partner.read') planReading = true;
          if (method === 'partner.stop') assert.ok(!notes && ++stopped <= 2);
          collectExited = notes ? fs.existsSync(path.join(root, 'revision-updated')) : stopped === 2;
          const status = notes ? 'done' : 'interrupted';
          const changes = (notes ? [1, 2] : [1]).map(x => ({ x, y: -61, z: 2, before: 'grass_block', after: 'air' }));
          const result = directPlace ? { status, placementAttempted: true, placed: true, sourceMessage: 2, taskRevision: 1,
            blocks: [{ ...wall[0], block: 'dirt' }], changes: [{ ...wall[0], before: 'air', after: 'dirt' }] } :
            { status, broken: changes.length, changes, inventoryDelta: { dirt: changes.length } };
          return { ...context, skill, exited: collectExited, result: collectExited ? result : {} };
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
        if (recovery && !stopped && method === 'partner.read') {
          if (replies) ++recoveredReads;
          else ++blockedReads;
          return { ...context, skill, exited: false, result: { progress: replies ?
            { phase: 'in_range', terrain: 'land', distance: 1.5, vertical: 0, path: 'reachable' } :
            { phase: 'blocked', terrain: 'land', distance: 8, vertical: 5, path: 'unreachable' } } };
        }
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
    return { generation, order: restartRoute ? 5 : first || motion || confirm || pointedCraft || attention ? 4 :
      interrupt || showGame || delayed || departure || (revision && !repairObserve && !repairPlan && !pointedSupport && !layered) ? 3 : 2,
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
      if (repairPlan) {
        assert.equal(replies, 1);
        const events = fs.readFileSync(path.join(root, 'trace', 'events.jsonl'), 'utf8').split('\n');
        assert.equal(events.filter(line => line.includes('"category":"mc.partner.plan"') &&
          line.includes('"status":"rejected"')).length, 2);
        assert.ok(events.some(line => line.includes('"kind":"run.exited"') && line.includes('"run":2,') &&
          !line.includes('work_pending') && line.includes('这次没动起来')));
      }
      console.log(path.join(root, 'trace', 'events.jsonl'));
    } else if (attention) {
      assert.equal(replies, 2);
      assert.equal(stopped, 1);
      const trace = path.join(root, 'trace', 'events.jsonl');
      const rows = fs.readFileSync(trace, 'utf8').trim().split('\n').map(line => JSON.parse(line));
      const alerts = rows.filter(row => row.kind === 'mail.received' && row.category === 'mc.partner.attention');
      assert.deepEqual(alerts.map(row => [row.body.status, row.body.work, row.body.episode]),
        [['raised', following, 1], ['recovered', following, 1]]);
      console.log(trace);
    } else if (layered) {
      assert.equal(nodeExits.length, layeredBlocked ? 2 : 4);
      assert.equal(replies, layeredBlocked ? 1 : 0);
      console.log(path.join(root, 'trace', 'events.jsonl'));
    } else if (revision) {
      if (correctionLost) assert.ok(stopped >= 2);
      else assert.equal(stopped, notes || restartRoute || pointedSupport || pointedCraft ? 0 : 2);
      assert.equal(collectExited, !correctionLost && !restartRoute);
      assert.equal(nextBlock, notes && !directRevision);
      assert.equal(wallFilled, !!correction && !correctionLost);
      if (restartRoute) assert.ok(restartWork && resumed && restartReads === 9 && routeStops === 1);
      else if (navigation) assert.equal(routeReads, 9);
      if (pointedCraft) assert.equal(replies, 3);
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
