const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { randomUUID, randomBytes } = require('node:crypto');
const { once } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const { openBridge } = require('../../plugins/mc/bridge/pipe.cjs');
const { fromGround, requiresCollectionApproval } = require('../../plugins/mc/bridge/collection.cjs');
const { BuiltObjects } = require('../../plugins/mc/bridge/built-objects.cjs');
const { readPartnerContext } = require('./partner-context.cjs');

// 核对一个前台工作的接纳和唯一退出，并检查它是否等到所需的前序回执。
// 步骤：按桥接工作号查接纳与退出 → 核对顺序并返回原记录。
function checkWorkReceipt(rows, work, after = 0) {
  const accepted = rows.find(row => row.kind === 'mail.received' && row.category === 'mc.partner.motion' &&
    row.body.status === 'accepted' && row.body.action.bridgeWork === work);
  const exits = rows.filter(row => row.kind === 'mail.received' && row.category === 'mc.partner.action' &&
    row.body.bridgeWork === work);
  assert.equal(exits.length, 1, `原工作应有一次退出 ${work}`);
  const [exit] = exits;
  assert.ok(accepted && after < accepted.seq && accepted.seq < exit.seq, `前台接纳与退出顺序 ${work}`);
  return { accepted, exit };
}

// 核对跟随执行段的退出，并确认前台交接沿用原逻辑工作号。
// 步骤：按执行段查回执 → 对照逻辑工作号与跟随状态。
function checkFollowReceipt(rows, work, following, transition, after = 0) {
  const { accepted, exit } = checkWorkReceipt(rows, work, after);
  assert.equal(accepted.body.action.work, following);
  assert.deepEqual([exit.body.work, exit.body.transition, exit.body.result.status], [following, transition, 'interrupted']);
  return exit;
}

// 按原工作号核对每层只有一个动作的测试计划。
// 步骤：定位模型收到的计划 → 对照信箱结算 → 核对退出与接纳顺序。
function checkPlanReceiptOrder(rows, works, after) {
  const turn = rows.filter(row => row.kind === 'model.prepared' && row.step === 1)
    .map(readPartnerContext).find(turn => turn.input.trigger === 'plan.finished');
  const plan = turn.input.taskPlan;
  const mail = rows.filter(row => row.kind === 'mail.received');
  const layers = mail.filter(row => row.category === 'mc.partner.plan' && row.body.status === 'settled' && row.body.plan === plan.id);
  assert.deepEqual(turn.input.planSettlements, layers.map(row => row.body));
  assert.equal(layers.length, plan.nodes.length);
  assert.ok(layers.length >= works.length);
  let previous = after;
  for (const [index, layer] of layers.entries()) {
    assert.equal(layer.body.layer, index + 1);
    assert.equal(layer.body.nodes.length, 1);
    const [node] = layer.body.nodes;
    assert.equal(node.work, plan.nodes[index].work);
    if (index < works.length) {
      assert.equal(node.work, works[index]);
      const { exit } = checkWorkReceipt(rows, node.work, previous);
      assert.ok(exit.seq < layer.seq);
      const { gameStatus, ...evidence } = node.result;
      if (gameStatus !== undefined) {
        assert.equal(evidence.status, 'blocked');
        assert.equal(evidence.reasonCode, 'pickup_unverified');
        delete evidence.reasonCode;
        evidence.status = gameStatus;
      }
      for (const [key, value] of Object.entries(evidence)) assert.deepEqual(value, exit.body.result[key]);
    } else {
      assert.equal(node.result.reason, 'prerequisite_failed');
      assert.ok(!mail.some(row => (row.category === 'mc.partner.motion' && row.body.action?.bridgeWork === node.work) ||
        (row.category === 'mc.partner.action' && row.body.bridgeWork === node.work)));
    }
    previous = layer.seq;
  }
  return layers;
}

// 核对同一局从玩家原话到实际发言的各段时序。
// 步骤：用消息号找模型回合 → 用请求号和工作号核对发言及动作 → 输出 Trace 上相邻阶段的耗时。
function checkResponseTiming(rows, works, delivered) {
  const mail = rows.filter(row => row.kind === 'mail.received');
  const message = order => mail.find(row => row.category === 'mc.partner.event' && row.body.event.sequence === order);
  const work = (category, id) => mail.find(row => row.category === category &&
    (row.body.status === 'accepted' ? row.body.action : row.body).bridgeWork === id);
  const turns = rows.filter(row => row.kind === 'model.prepared' && row.step === 1)
    .map(row => ({ row, input: readPartnerContext(row).input }));
  const original = message(2), question = message(3);
  const outcomes = [original, question].map(event => turns.find(turn =>
    turn.input.events.some(input => input.sequence === event.body.event.sequence)));
  outcomes.push(turns.find(turn => turn.input.trigger === 'plan.finished'));
  assert.ok(outcomes.every(Boolean));
  assert.deepEqual(outcomes.slice(0, 2).map(turn => turn.input.events[0]), [original.body.event, question.body.event]);
  const requests = outcomes.map(({ row: { session, run } }) =>
    rows.find(row => row.kind === 'subagent.accepted' && row.session === session && row.run === run));

  const replies = mail.filter(row => row.category === 'mc.partner.action' && row.body.skill === 'reply');
  assert.deepEqual(replies.map(({ body }) => [body.bridgeWork, body.order, body.result.text, body.result.sent]),
    delivered.map(({ id, order, text }) => [id, order, text, true]));
  assert.deepEqual(replies.map(({ body }) => [body.session, body.request]), requests.map(row => [row.session, row.request]));
  assert.deepEqual(replies.map(({ body }) => body.source ?? 'tool'), ['tool', 'final_text', 'final_text']);
  const [firstReply, liveReply, finalReply] = replies;
  const firstTool = rows.find(row => row.kind === 'tool.exited' && row.callId === 'timing-first-reply');
  assert.deepEqual([firstReply.body.session, firstReply.body.invocation], [firstTool.session, firstTool.invocation]);
  const [first, second] = works;
  const completion = checkPlanReceiptOrder(rows, works, firstReply.seq).at(-1);
  const firstAction = work('mc.partner.motion', first), firstExit = work('mc.partner.action', first);
  const secondExit = work('mc.partner.action', second);
  for (const exit of [firstExit, secondExit]) {
    assert.deepEqual([exit.body.order, exit.body.session, exit.body.request, exit.body.result.arrived],
      [2, requests[0].session, requests[0].request, true]);
  }
  const liveModel = kind => rows.find(row => row.kind === kind && row.session === requests[1].session && row.run === requests[1].run);
  const timeline = [original, firstReply, firstAction, question, firstExit, liveReply, secondExit, completion, finalReply];
  assert.ok(timeline.every((row, i) => !i || (timeline[i - 1].seq < row.seq && timeline[i - 1].timeNs <= row.timeNs)));
  assert.ok(liveModel('model.submitted').seq < firstExit.seq && firstExit.seq < liveModel('model.exited').seq);
  const elapsedMs = (start, end) => Math.round((end.timeNs - start.timeNs) / 1e6);
  console.log('response-timing', JSON.stringify({ messageToFirstReplyMs: elapsedMs(original, firstReply),
    firstReplyToActionMs: elapsedMs(firstReply, firstAction), actionToCompletionMs: elapsedMs(firstAction, secondExit),
    completionToReplyMs: elapsedMs(secondExit, finalReply), questionToReplyMs: elapsedMs(question, liveReply) }));
}

// 提供固定地面与登记过的对象，复用执行器的取材和保护判定。
// 步骤：构造方格和掉落表 → 登记已建对象 → 返回候选及现场读口。
function refusedGround() {
  const position = (x, y, z) => ({ x, y, z, offset: (dx, dy, dz) => position(x + dx, y + dy, z + dz) });
  const dirt = { id: 3, type: 3, name: 'dirt', drops: [3], boundingBox: 'block' };
  const coarse = { ...dirt, id: 4, type: 4, name: 'coarse_dirt' };
  const candidates = [2, 3, 4, 5].map(x => ({ ...(x < 4 ? dirt : coarse), position: position(x, -61, 0) }));
  const built = new BuiltObjects();
  built.bind({});
  built.placed([{ x: 4, y: -61, z: 0, block: 'coarse_dirt' }]);
  return {
    registry: { blocksByName: { dirt, coarse_dirt: coarse }, itemsByName: { dirt: { id: 3 } } },
    players: {}, candidates, built,
    blockAt: ({ x, y, z }) => y === -61 && z === 0 && x >= 4 ? coarse : y <= -61 ? dirt : { name: 'air' },
  };
}

// 用独立目录的插件配置把真实 DLL 接到受控世界来源。
// 步骤：签发管道短名并保存用户设置 → 运行原生安装与执行探针 → 核对调用数并关闭。
async function main() {
  const [exe, packagePath, temp, variant] = process.argv.slice(2);
  const boardWindow = variant === 'board-window';
  const controllerRestart = variant === 'partner-controller-restart';
  const interrupt = variant === 'partner-interrupt';
  const suspend = variant?.startsWith('partner-follow-collect');
  const recovery = variant === 'partner-follow-recovery';
  const follow = suspend || recovery || variant === 'partner-follow';
  const motion = follow || variant === 'partner-motion';
  const confirm = variant === 'partner-confirm';
  const refusedCollection = variant?.startsWith('partner-refused-collection');
  const scene = variant === 'partner-scene';
  const departure = variant === 'partner-departure';
  const execution = variant?.startsWith('partner-execution-');
  const scenario = variant === 'partner-execution-scenario';
  const delivery = variant?.startsWith('partner-reply-') ? variant.slice('partner-reply-'.length) : null;
  const revision = controllerRestart || variant?.startsWith('partner-revision-');
  const directRevision = controllerRestart || variant?.startsWith('partner-revision-direct-');
  const directPlace = variant?.startsWith('partner-revision-direct-place');
  const stalePlace = variant === 'partner-revision-direct-place-stale';
  const rejoin = variant === 'partner-revision-direct-rejoin';
  const recoverInbox = variant === 'partner-revision-direct-recover-notes';
  const notes = revision && variant.endsWith('-notes');
  const correction = variant?.startsWith('partner-revision-correction');
  const correctionLost = variant === 'partner-revision-correction-lost';
  const navigation = variant?.startsWith('partner-revision-navigation');
  const restartRoute = variant === 'partner-revision-navigation-restart';
  const mixedRoute = variant === 'partner-revision-navigation-mixed';
  const repairObserve = variant === 'partner-revision-repair-observe';
  const repairPlan = variant === 'partner-revision-repair-plan';
  const repairPlanObserve = variant === 'partner-revision-repair-plan-observe';
  const observationBatch = variant === 'partner-revision-observation-batch';
  const responseTiming = variant === 'partner-revision-response-timing';
  const layered = variant?.startsWith('partner-revision-layered');
  const layeredStock = variant === 'partner-revision-layered-stock';
  const layeredBlocked = variant === 'partner-revision-layered-blocked';
  const layeredStepCount = layeredStock ? 3 : layeredBlocked ? 2 : 4;
  const attention = variant === 'partner-revision-attention';
  const questionScope = variant === 'partner-revision-question-scope';
  const openingAnswer = variant === 'partner-revision-opening-answer';
  const continuationBudget = variant === 'partner-revision-continuation-budget';
  const questionExpiry = variant === 'partner-revision-question-expiry';
  const observeDigScope = variant?.startsWith('partner-revision-observe-dig-scope');
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
  const partnerLoop = variant === 'partner-loop' || interrupt || motion || confirm || refusedCollection || task || route || exchange || departure || revision || execution || delivery;
  fs.mkdirSync(temp, { recursive: true });
  let root = fs.mkdtempSync(path.join(temp, 'world-'));
  const config = path.join(root, 'user', 'plugins', 'mc.companion');
  fs.mkdirSync(config, { recursive: true });
  const channel = randomUUID();
  const key = randomBytes(32).toString('hex');
  fs.writeFileSync(path.join(config, 'settings.json'), JSON.stringify({ channel, bridgeKey: key }));
  let reads = 0;
  let baselineRead = false, pendingChat = false, replies = 0, stopped = 0, releases = 0, moveId, approvedId;
  let controllerOwner;
  let following, collecting, craftingWork, resumed, collectExited = false;
  let pickupPending = false, placementQuestionWork;
  const sceneReads = [];
  let meetingWork;
  const timingWorks = [], timingSpeech = [];
  let timingFirstRead = false;
  let blockedReads = 0, recoveredReads = 0;
  let attentionSamples = 0;
  let planReading = false, nextBlock = false;
  let failedReads = 0, workReadFailures = 0, lastWorldRead = 0;
  let regressScene = false;
  const nodeExits = [];
  let nodeAtWork, nodeReads;
  let wallFilled = false;
  let badObservations = 0;
  let questionPresented = false, expiredAt;
  let clarificationAck = false;
  let heardAt;
  let surveyId, checkedRoute, routeWork, routeReads = 0, oldCheckpoint = -1;
  let restartWork, restartReads = 0, routeStops = 0, followIsRunning = false, resumedFollowRead = false;
  let followPauseReads = 0, oldRouteExited = false, routeCompleted = false;
  let routeApproach, routeDeparture;
  const hostWork = [];
  let explicitReply, replyReads = 0;
  const collectionWorks = new Map();
  const waypoints = [[1, -6], [4, -6], [4, -4], [1, -4]].map(([x, z]) => ({ x, y: -60, z }));
  const state = { generation: randomUUID(), actor: randomUUID(), dimension: 'overworld',
    position: { x: -1.5, y: -60, z: 0.5 }, cobblestone: 5, signalAgeMs: 20 };
  const boundaryAnchor = { dimension: 'overworld', sampledAtMs: 42,
    position: { x: -0.000041, y: -60.000041050085805, z: 9.99996 },
    facing: { yaw: 0.1234567, pitch: -0.9876543 } };
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
        if (recoverInbox && planReading && failedReads < 3) {
          assert.equal(after, 2, 'same world must resume from its original cursor');
          assert.ok(!failedReads || reads > lastWorldRead, 'resume must revalidate the world before reading the inbox');
          lastWorldRead = reads;
          if (++failedReads === 1) throw new Error('simulate one disconnected chat.read');
          regressScene = failedReads === 2;
        }
        if (questionExpiry && questionPresented && expiredAt === undefined) {
          const trace = path.join(root, 'trace', 'events.jsonl');
          if (fs.existsSync(trace) && fs.readFileSync(trace, 'utf8').split('\n').some(line =>
            line.includes('"category":"mc.partner.question"') && line.includes('"status":"expired"'))) expiredAt = performance.now();
        }
        const latest = initial ? 1 :
          refusedCollection ? (!approvedId || !fs.existsSync(path.join(root, 'collection-pending-checked')) ? 2 :
            after < 3 ? 3 : fs.existsSync(path.join(root, 'collection-receipt-2')) ? 6 :
            fs.existsSync(path.join(root, 'collection-receipt-1')) ? 5 : 4) :
          repairPlanObserve ? (fs.existsSync(path.join(root, 'observe-plan-settled')) ? 4 :
            fs.existsSync(path.join(root, 'observe-plan-rejected')) ? 3 : 2) :
          responseTiming ? (timingFirstRead ? 3 : 2) :
          rejoin ? (planReading ? 5 : 2) :
          restartRoute ? (oldCheckpoint >= 5 ? 5 : oldCheckpoint >= 2 ? 4 : followIsRunning ? 3 : 2) :
          layered ? (followIsRunning ? 3 : 2) :
          openingAnswer ? (questionPresented ? 3 : 2) :
          questionScope ? (questionPresented && fs.existsSync(path.join(root, 'scoped-question-checked')) ? 3 : 2) :
          pointedCraft ? (heardAt !== undefined && performance.now() - heardAt > 300 ? 4 : questionPresented ? 3 : 2) :
          attention ? (replies === 2 ? 4 : recoveredReads >= 8 ? 3 : 2) :
          directPlace ? (planReading ? 5 : !questionPresented ? 2 : fs.existsSync(path.join(root, 'placement-waiting-checked')) ? 4 : 3) :
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
        if (refusedCollection) {
          messages[1].text = '先问我，再从地面把泥土补到一个';
          messages[1].sceneAtMessage.position = { x: 2, y: -60, z: 2 };
          messages[2].text = '拒绝 ' + approvedId;
          messages[3].text = '再检查一下原来的取材';
          messages[4].text = '再检查用泥土补库存';
          messages.push({ sequence: 6, text: '从没有保护的表层砂土补齐' });
        }
        if (revision) {
          messages[1].text = '清理门边两格草方块';
          if (observationBatch) {
            messages[1].text = '看看我在哪，能找到就过来';
            messages[1].sceneAtMessage = boundaryAnchor;
          }
          if (attention) {
            messages[1].text = '大肥鱼，跟着我';
            messages[2].text = '你听得到吗';
            messages[3].text = '不用跟了';
          }
          if (correction || navigation || directPlace) {
            messages[1].text = '把我面前墙上的四格补上';
            messages[1].sceneAtMessage.position = { x: 3, y: -60, z: -7 };
          }
          messages[2].text = navigation ? '现在绕着这个墙跑两圈' : correction ? '我说的是墙，你应该竖着补' :
            notes ? '补记一下角落，接着做' : variant === 'partner-revision-goal' ? '改成清理另一处' :
            controllerRestart || variant.endsWith('-scope') ? '只从地面取材' : '这次采掘先问我确认';
          if (responseTiming) {
            messages[1].text = '走到拐角，再去门边。';
            messages[2].text = '还在走吗？';
          }
          if (layered) {
            messages[1].text = '大肥鱼，跟着我';
            messages[2].text = '你可以垫上来';
            messages[2].sceneAtMessage = { ...messages[1].sceneAtMessage, position: { x: 3.5, y: -60, z: 25.5 } };
          }
          if (directPlace) {
            messages[2].text = stalePlace ? '改成清理另一处' : '等我选一下';
            messages[3].text = '左边';
            messages[4].text = '改成清理另一处';
          }
          if (questionExpiry) {
            messages[1].text = '把我面前的挖掉';
            messages[2].text = '你还在吗';
          }
          if (questionScope) {
            messages[1].text = '挖一块泥土来垫脚';
            messages[2].text = '还是用泥土';
          }
          if (openingAnswer) {
            messages[1].text = '把墙上的口补一下';
            messages[1].sceneAtMessage.position = { x: 3, y: -60, z: -7 };
            messages[2].text = '两边都补上吧';
          }
          if (repairPlanObserve) {
            messages[1].text = '先列个观察计划。';
            messages[2].text = '把观察计划修好，再看看周围。';
            messages[3].text = '看完了吗？';
          }
          if (pointedSupport) messages[1].text = '把这石块上的泥土敲掉';
          if (observeDigScope) messages[1].text = '就近取一块泥土';
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
        if (rejoin) {
          messages[2] = left;
          messages[3] = { sequence: 4, type: 'player.entered' };
          messages[4].text = '我回来了，先聊一会';
        }
        return { generation, owner: 'fff01234-1234-1234-1234-123456789abc', oldest: 1, latest,
          gap: false, more: false, presence: rejoin && latest === 5 ? messages[3] : departure && latest === 3 ? left : entered,
          ...(rejoin && { lastDeparture: latest === 5 ? 3 : 0 }), messages: messages.slice(after, latest) };
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
  }, key, partnerLoop ? { partner: scenario ? null : {
    // 在原生测试中模拟桥接授予同一个实例控制权。
    claim(controller) {
      if (controllerOwner && controllerOwner !== controller) return false;
      controllerOwner = controller;
      return true;
    },
    // 核对原宿主的局部释放，第二次查询返回实际退出。
    detach(controller) {
      assert.equal(controller, controllerOwner);
      if (++releases === 1) return 'requested';
      controllerOwner = undefined;
      return 'released';
    },
    // 受控游戏场景只有在原任务完成登记后才产生新修订。
    scene(generation) {
      assert.equal(generation, state.generation);
      if (recoverInbox) return regressScene ? 1 : 2;
      return scene ? (fs.existsSync(path.join(root, 'scene-changed')) ? 2 : 1) : null;
    },
    // 验证来自原回合的真实工具参数，并返回游戏实际发送的替身回执。
    call(method, id, context, skill, args) {
      assert.ok(!execution, '不可执行时不应调用游戏技能');
      const { controller } = context;
      assert.match(controller, /^[a-f0-9]{32}$/);
      assert.equal(controller, controllerOwner);
      if (refusedCollection) {
        if (skill === 'reply') {
          assert.equal(method, 'partner.begin');
          assert.equal(context.order, 2);
          const match = /确认 (\d+) 或 拒绝 \1$/.exec(args.text);
          assert.ok(match && !approvedId);
          approvedId = match[1];
          return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
        }
        assert.equal(skill, 'collect');
        if (method === 'partner.read') {
          assert.ok(collectionWorks.has(id));
          return { ...context, skill, exited: true, result: collectionWorks.get(id) };
        }
        assert.equal(method, 'partner.begin');
        assert.equal(context.order, 4 + collectionWorks.size);
        const target = [{ x: 3, y: -60, z: 0 }];
        assert.equal(args.source, 'ground');
        assert.equal(args.count, 1);
        assert.deepEqual(args.protectedCells, target);
        assert.deepEqual(args.collectionGuards, [{ item: 'dirt', block: 'dirt', source: 'ground', protectedCells: target }]);
        const ground = refusedGround();
        const [refused, support, built, spare] = ground.candidates;
        const stock = context.order >= 5;
        if (stock) assert.equal(args.requiredStock, 1);
        else assert.deepEqual([args.x, args.y, args.z], [2, -61, 0]);
        const candidates = !stock ? [refused] : args.block === 'dirt' ? [support, refused] : [built, spare];
        const selected = candidates.find(block => block.name === args.block && fromGround(ground, block, args.protectedCells) &&
          ground.built.permits([block], false, ground.blockAt));
        assert.ok(selected, 'fixture needs an unprotected candidate');
        const blocked = requiresCollectionApproval(ground, args.collectionGuards, [selected]);
        const { x, y, z } = selected.position;
        collectionWorks.set(id, blocked ? { status: 'blocked', reasonCode: 'approval_required', broken: 0, changes: [], inventoryDelta: {} } :
          { status: 'done', broken: 1, changes: [{ x, y, z, before: selected.name, after: 'air' }],
            inventoryDelta: { dirt: 1 }, stock: { item: 'dirt', before: 0, required: 1, after: 1 } });
        return { ...context, skill, exited: false, result: {} };
      }
      if (controllerRestart) {
        if (method === 'partner.begin') hostWork.push({ id, context, args });
        else assert.deepEqual({ id, context, args }, hostWork.at(-1));
      }
      if (delivery) {
        assert.equal(context.order, 2);
        if (delivery === 'finish' && skill === 'observe') {
          assert.equal(method, 'partner.begin');
          assert.deepEqual(args, {});
          return { ...context, skill, exited: true, result: { status: 'done', position: state.position } };
        }
        assert.equal(skill, 'reply');
        assert.ok(!Object.hasOwn(args, 'finish'));
        const text = '我在这儿。';
        if (id.startsWith('reply.')) {
          assert.ok(explicitReply && delivery === 'fallback');
          assert.equal(method, 'partner.begin');
          assert.equal(args.text, text);
          assert.equal(++replies, 1);
          return { ...context, skill, exited: true, result: { status: 'done', sent: true, delivery: 'sent', text } };
        }
        if (method === 'partner.begin') {
          assert.equal(explicitReply, undefined);
          explicitReply = { id, context, args };
          if (delivery === 'unknown') throw new Error('模拟接纳回复后丢失传输响应');
        } else {
          assert.equal(delivery, 'unknown');
          assert.equal(method, 'partner.read');
          assert.deepEqual({ id, context, args }, explicitReply);
          assert.equal(++replyReads, 1);
        }
        if (delivery === 'fallback' || delivery === 'internal') {
          assert.equal(args.text, '第一行。\n第二行。');
          return { ...context, skill, exited: true, result: { status: 'blocked', sent: false, delivery: 'not_sent', reason: '回复需要一行' } };
        }
        assert.equal(args.text, text);
        if (delivery === 'unknown') return { ...context, skill, exited: true, result: { status: 'blocked', sent: false, delivery } };
        assert.equal(++replies, 1);
        return { ...context, skill, exited: true, result: { status: 'done', sent: true, delivery: 'sent', text } };
      }
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
            recoveredReads > 1 ? { phase: 'in_range', terrain: 'dry', distance: 2.8, vertical: 2.8, path: 'reachable' } :
            attentionSamples <= 3 ? { phase: 'following', terrain: 'swimming', distance: 4, vertical: 0.5, path: 'reachable' } :
              { phase: replies ? 'blocked' : 'following', terrain: 'dry', distance: 8, vertical: 5, path: 'unreachable' };
          return { ...context, skill, exited: false, result: { progress: { ...progress, sampledAtMs } } };
        }
        if ((layered || restartRoute) && skill === 'follow') {
          if (method === 'partner.begin') {
            if (!following) following = id;
            else {
              if (layered) assert.equal(nodeExits.length, layeredStepCount);
              else assert.equal(restartReads, 9);
              assert.equal(id, following + '.resume.2');
              resumed = id;
            }
          }
          assert.equal(id, resumed || following);
          if (method === 'partner.read') {
            followIsRunning = true;
            if (resumed && !resumedFollowRead) {
              resumedFollowRead = true;
              fs.writeFileSync(path.join(root, 'original-follow-resumed'), '');
            }
          }
          if (method !== 'partner.stop' || (id === following && ++followPauseReads === 1))
            return { ...context, skill, exited: false, result: {} };
          return { ...context, skill, exited: true, result: { status: 'interrupted' } };
        }
        if (layered) {
          assert.equal(context.order, 3);
          if (layeredStepCount < 4 && skill === 'reply') {
            assert.equal(method, 'partner.begin');
            assert.equal(args.text, layeredStock ? '第一块垫好了，背包没有第二块了。' : '第一块垫好了，但我站不上去。');
            assert.equal(nodeExits.length, layeredStepCount);
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
          assert.ok(index < layeredStepCount);
          assert.equal(followPauseReads, 2);
          assert.equal(skill, place ? 'place' : 'move');
          if (method === 'partner.begin') {
            assert.ok(!nodeAtWork && !nodeExits.includes(id));
            assert.deepEqual(args, place ? { ...pad, block: 'dirt', sourceMessage: 3, taskRevision: 2 } :
              { ...pad, y: pad.y + 1, exact: true });
            nodeAtWork = id;
            nodeReads = 0;
            return { ...context, skill, exited: false, result: {} };
          }
          assert.equal(method, 'partner.read');
          assert.equal(id, nodeAtWork);
          if (nodeReads++ === 0) return { ...context, skill, exited: false, result: {} };
          nodeExits.push(id);
          nodeAtWork = undefined;
          if (layeredStock && index === 2) return { ...context, skill, exited: true,
            result: { status: 'blocked', reasonCode: 'material_missing', placementAttempted: false, placed: false } };
          const result = place ? { status: 'done', placementAttempted: true, placed: true,
            sourceMessage: 3, taskRevision: 2, blocks: [{ ...pad, block: 'dirt' }],
            changes: [{ ...pad, before: 'air', after: 'dirt' }], materialBefore: 2 - index / 2, materialAfter: 1 - index / 2 } :
            layeredBlocked ? { status: 'blocked', reasonCode: 'unsafe_stand' } :
              { status: 'done', arrived: true, position: { x: pad.x + 0.5, y: pad.y + 1, z: pad.z + 0.5 } };
          return { ...context, skill, exited: true, result };
        }
        if (continuationBudget) {
          assert.equal(context.order, 2);
          assert.equal(skill, 'observe');
          assert.equal(method, 'partner.begin');
          assert.deepEqual(args, {});
          assert.ok(!sceneReads.includes(id));
          sceneReads.push(id);
          return { ...context, skill, exited: true, result: { status: 'done', position: state.position } };
        }
        if (openingAnswer) {
          assert.equal(method, 'partner.begin');
          if (skill === 'reply') {
            assert.ok(!questionPresented);
            assert.equal(context.order, 2);
            assert.equal(args.text, '补哪些缺口？');
            questionPresented = true;
            return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
          }
          if (skill === 'observe' && args.sources.includes('message')) {
            assert.equal(args.message, 2);
            const candidates = context.order === 2 ? [2, 4].map((x, i) => ({ id: i ? 'right' : 'left', x, y: -60, z: -5,
              width: 1, height: 2, axis: 'x' })) : [{ id: 'other', x: 3, y: -60, z: -4, width: 1, height: 2, axis: 'x' }];
            return { ...context, skill, exited: true, result: { status: 'done', anchor: { message: 2 }, sampledAtMs: 42,
              playerFocus: { choice: null, status: 'ambiguous', candidates } } };
          }
          assert.equal(context.order, 3);
          const cells = [2, 4].flatMap(x => [-60, -59].map(y => ({ x, y, z: -5 })));
          if (skill === 'observe') {
            assert.deepEqual(args.verify, cells);
            return { ...context, skill, exited: true, result: { status: 'done', observation: {
              id, dimension: 'overworld', generation: context.generation, requested: 1, samples: [{ sampledAtMs: 84,
                views: [{ source: 'self', status: 'sampled', actor: { position: { x: 3, y: -60, z: -6 } } }],
                verified: cells.map(cell => ({ ...cell, atSelf: true, block: 'air' })) }],
            } } };
          }
          assert.equal(skill, 'fill_target');
          assert.ok(!wallFilled);
          assert.deepEqual(args, { cells: [cells[0], cells[2], cells[1], cells[3]], block: 'dirt', sourceMessage: 2, taskRevision: 1 });
          wallFilled = true;
          return { ...context, skill, exited: true, result: { status: 'done', sourceMessage: 2, taskRevision: 1, placementAttempted: true,
            cells: cells.map(cell => ({ ...cell, before: 'air', block: 'dirt', status: 'placed' })),
            changes: cells.map(cell => ({ ...cell, before: 'air', after: 'dirt' })) } };
        }
        if (questionScope) {
          assert.equal(context.order, 2);
          if (skill === 'reply') {
            assert.equal(method, 'partner.begin');
            assert.ok(!questionPresented);
            assert.equal(args.text, '草方块也可以吗？');
            questionPresented = true;
            return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
          }
          assert.equal(skill, 'collect');
          assert.deepEqual(args, { x: 1, y: -61, z: 3, count: 1, block: 'dirt', source: 'visible' });
          if (method === 'partner.begin') {
            assert.ok(!collecting);
            collecting = id;
            return { ...context, skill, exited: false, result: {} };
          }
          assert.equal(method, 'partner.read');
          assert.equal(id, collecting);
          collectExited = true;
          return { ...context, skill, exited: true, result: { status: 'done', broken: 1, inventoryDelta: { dirt: 1 },
            changes: [{ x: 1, y: -61, z: 3, before: 'dirt', after: 'air' }] } };
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
        if (repairPlanObserve) {
          assert.equal(method, 'partner.begin');
          if (skill === 'observe') {
            assert.equal(context.order, 3);
            assert.deepEqual(args, {});
            assert.deepEqual(nodeExits, []);
            nodeExits.push(id);
            return { ...context, skill, exited: true, result: { status: 'done', sampledAtMs: 42, position: state.position } };
          }
          assert.equal(skill, 'reply');
          assert.equal(context.order, 4);
          assert.equal(args.text, '看过了，我还在。');
          assert.equal(++replies, 1);
          return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
        }
        if (responseTiming) {
          if (skill === 'reply') {
            assert.equal(method, 'partner.begin');
            assert.deepEqual([context.order, args.text], [[2, '好，我现在过去。'], [3, '还在走，接着去门边。'],
              [3, '两段都走完了。']][timingSpeech.length]);
            timingSpeech.push({ id, order: context.order, text: args.text });
            return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
          }
          assert.equal(skill, 'move');
          if (method === 'partner.begin') {
            assert.equal(context.order, timingWorks.length === 0 ? 2 : 3);
            assert.deepEqual(args, { x: timingWorks.length ? 4 : 2, y: -60, z: 1, exact: true });
            timingWorks.push(id);
            return { ...context, skill, exited: false, result: {} };
          }
          assert.equal(method, 'partner.read');
          assert.equal(id, timingWorks.at(-1));
          assert.equal(context.order, timingWorks.length === 1 ? 2 : 3);
          if (timingWorks.length === 1) {
            timingFirstRead = true;
            if (!fs.existsSync(path.join(root, 'timing-chat-open'))) return { ...context, skill, exited: false, result: {} };
          } else if (timingSpeech.length < 2) return { ...context, skill, exited: false, result: {} };
          return { ...context, skill, exited: true, result: { status: 'done', arrived: true,
            position: { x: args.x + 0.5, y: -60, z: 1.5 } } };
        }
        if (observationBatch) {
          assert.equal(context.order, 2);
          if (skill === 'observe') {
            assert.equal(method, 'partner.begin');
            assert.deepEqual(args, { mode: 'scene', sources: ['self', 'player', 'message'], message: 2, radius: 1 });
            assert.ok(!sceneReads.includes(id));
            sceneReads.push(id);
            const livePlayer = sceneReads.length > 1;
            return { ...context, skill, exited: true, result: { status: 'done', observation: {
              id, generation: context.generation, dimension: 'overworld', requested: 1, samples: [{
                sampledAtMs: livePlayer ? 242 : 142, views: [
                  { source: 'self', status: 'sampled', actor: { position: state.position } },
                  { source: 'player', status: livePlayer ? 'sampled' : 'unavailable',
                    actor: livePlayer ? { position: { x: 2.5, y: -60, z: 1.25 } } : null },
                  { source: 'message', status: 'sampled', message: 2, anchor: boundaryAnchor },
                ],
              }],
            } } };
          }
          assert.equal(skill, 'move');
          assert.equal(sceneReads.length, 2);
          assert.deepEqual(args, { x: 2, y: -60, z: 1, exact: true });
          if (method === 'partner.begin') {
            assert.equal(meetingWork, undefined);
            meetingWork = id;
            return { ...context, skill, exited: false, result: {} };
          }
          assert.equal(method, 'partner.read');
          assert.equal(id, meetingWork);
          return { ...context, skill, exited: true, result: { status: 'done', arrived: true,
            position: { x: 2.5, y: -60, z: 1.25 } } };
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
                assert.equal(args.text, noPickup ? '木头敲下来了，我还没捡到。' : '一根原木做成了四块木板。');
                break;
              default: assert.fail('unexpected repeated game reply');
            }
            return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
          }
          if (skill === 'craft') {
            assert.ok(!noPickup && collectExited);
            assert.equal(context.order, 4);
            assert.deepEqual(args, { item: 'oak_planks', ingredient: 'oak_log', count: 4 });
            if (method === 'partner.begin') {
              assert.equal(craftingWork, undefined);
              craftingWork = id;
              return { ...context, skill, exited: false, result: {} };
            }
            assert.equal(method, 'partner.read');
            assert.equal(id, craftingWork);
            return { ...context, skill, exited: true, result: { status: 'done', item: 'oak_planks', count: 4,
              craftAttempted: true, inventoryDelta: { oak_log: -1, oak_planks: 4 }, craft: { ingredient: 'oak_log',
                perBatch: 4, requiresTable: false, batches: 1, completedBatches: 1, consumed: 1, produced: 4, remaining: 0 } } };
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
          const { block, ...position } = log;
          const changes = [{ ...position, before: block, after: 'air' }];
          if (!pickupPending) {
            pickupPending = true;
            return { ...context, skill, exited: false, result: { broken: 1, changes } };
          }
          collectExited = true;
          return { ...context, skill, exited: true, result: { status: 'done', broken: 1, changes,
            inventoryDelta: noPickup ? {} : { oak_log: 1 } } };
        }
        if (pointedSupport || observeDigScope) {
          const stone = { x: 1, y: -60, z: 2, block: 'stone' };
          const dirt = { x: 1, y: -59, z: 2, block: 'dirt' };
          assert.equal(context.order, 2);
          if (skill === 'observe') {
            assert.equal(method, 'partner.begin');
            if (variant.includes('-evidence')) {
              const { block, ...cell } = dirt;
              const unloaded = { ...cell, z: 3 };
              assert.deepEqual(args, { mode: 'scene', sources: ['self'], verify: [cell, unloaded] });
              return { ...context, skill, exited: true, result: { status: 'done', observation: {
                id, generation: context.generation, dimension: 'overworld', requested: 1, samples: [{ sampledAtMs: 42,
                  verified: [{ ...dirt, atSelf: true }, { ...unloaded, atSelf: true, block: null }],
                  views: [{ source: 'self', status: 'sampled', actor: { position: { x: 0.5, y: -59, z: 2.5 } } }],
                }],
              } } };
            }
            assert.deepEqual(args, { mode: 'pointed', message: 2, focus: 'objects' });
            return { ...context, skill, exited: true, result: { status: 'done', anchor: { message: 2 }, sampledAtMs: 42,
              playerFocus: { status: 'observed', hit: stone, objects: [{ ...dirt, visible: true, below: 'stone' }] } } };
          }
          assert.equal(skill, 'collect');
          if (method === 'partner.begin') {
            assert.ok(!collecting);
            assert.deepEqual(args, { ...dirt, count: 1, source: 'visible', ...(pointedSupport ? { digReference: stone } : {}) });
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
        if (restartRoute && skill === 'reply') {
          assert.ok(routeWork && !restartWork && !questionPresented);
          assert.equal(context.order, 4);
          assert.equal(args.text, '要从这圈开始计时吗？');
          questionPresented = true;
          return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
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
          if (context.order === 5) assert.ok(oldRouteExited);
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
              assert.ok(restartRoute && oldRouteExited && !restartWork);
              restartWork = id;
            } else routeWork = id;
            if (restartRoute) assert.equal(followPauseReads, 2);
            assert.deepEqual(args, { route: checkedRoute, taskId: 3, taskRevision: restartRoute && !restartWork ? 1 : 2, waypoints, laps: 2 });
          }
          assert.equal(id, restartWork || routeWork);
          if (method === 'partner.stop') {
            assert.ok(restartRoute && !restartWork && ++routeStops <= 2);
            oldRouteExited = routeStops === 2;
          }
          const checkpoint = restartWork ? restartReads++ : Math.min(routeReads++, restartRoute ? questionPresented ? 5 : 2 : 8);
          if (!restartWork) oldCheckpoint = checkpoint;
          const checkpoints = Array.from({ length: checkpoint + 1 }, (_, index) =>
            ({ index, position: { ...waypoints[index % 4], x: waypoints[index % 4].x + 0.5 } }));
          const laps = Math.floor(checkpoint / 4);
          const status = method === 'partner.stop' ? oldRouteExited ? 'interrupted' : null :
            laps === 2 && (!mixedRoute || routeReads === 10) ? 'done' : null;
          if (status === 'done') routeCompleted = true;
          return { ...context, skill, exited: Boolean(status), result: {
            ...(status && { status }),
            progress: { phase: 'route', checkpoint, laps, requested: 2 },
            route: { id: checkedRoute, requested: 2, laps, checkpoints },
          } };
        }
        if (mixedRoute && skill === 'move') {
          if (method === 'partner.begin') {
            assert.ok(collectExited && checkedRoute && !routeDeparture);
            if (routeApproach) {
              assert.ok(routeCompleted);
              routeDeparture = id;
            } else routeApproach = id;
          } else assert.equal(method, 'partner.read');
          assert.equal(id, routeDeparture || routeApproach);
          const waypoint = waypoints[routeDeparture ? 1 : 0];
          assert.deepEqual(args, { ...waypoint, exact: true });
          if (method === 'partner.begin') return { ...context, skill, exited: false, result: {} };
          return { ...context, skill, exited: true, result: { status: 'done', arrived: true,
            position: { ...waypoint, x: waypoint.x + 0.5 } } };
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
        if (directPlace && skill === 'reply') {
          assert.equal(context.order, 2);
          assert.equal(args.text, '补左边还是右边？');
          assert.equal(placementQuestionWork, undefined);
          placementQuestionWork = id;
          questionPresented = true;
          return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
        }
        if (directRevision) {
          assert.ok(!stalePlace, 'a superseded answer must not reach the game');
          assert.equal(skill, directPlace ? 'place' : 'collect');
          assert.equal(context.order, directPlace ? 4 : 2);
          if (method === 'partner.begin') {
            assert.equal(collecting, undefined);
            assert.deepEqual(args, directPlace ? { ...wall[0], block: 'dirt', sourceMessage: 2, taskRevision: 1 } :
              { block: 'grass_block', count: 2 });
            collecting = id;
          }
          assert.equal(id, collecting);
          if (method === 'partner.read') {
            planReading = true;
            if (recoverInbox && failedReads > 0 && failedReads < 3) {
              ++workReadFailures;
              throw new Error('simulate the same outage while reading the original work');
            }
          }
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
          assert.deepEqual(context, { generation: state.generation, order: 3, controller });
          pendingChat = true;
          return { ...context, skill, exited: true, result: { status: 'done', sent: true, text: args.text } };
        }
        assert.deepEqual(context, { generation: state.generation, order: 2, controller });
        const match = /确认 (\d+) 或 拒绝 \1$/.exec(args.text);
        assert.ok(match && !approvedId);
        approvedId = match[1];
        return { ...context, skill, exited: true, result: { status: 'done', sent: true, reused: false, text: args.text } };
      }
      if (confirm || route) {
        assert.ok(!first && !delayed, 'a delivered approval must not restart a stale candidate');
        assert.equal(method, 'partner.begin');
        assert.equal(skill, 'collect');
        assert.deepEqual(context, { generation: state.generation, order: confirm ? 4 : showGame ? 3 : 2, controller });
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
        assert.deepEqual(context, { generation: state.generation, order: 2, controller });
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
      assert.deepEqual(context, { generation: state.generation, order: 2, controller });
      assert.equal(skill, 'reply');
      assert.deepEqual(args, { text: exchange ? 'delegated hello' : 'ready to play' });
      ++replies;
      return { ...context, skill, exited: true, result: { status: 'done', sent: true, reused: false, text: args.text } };
    },
  },
  // 模拟已释放控制的角色重生，原代次的接管票据不出现在本次收件。
  execution() { return execution && !scenario ? { mode: 'partner', state: 'released' } : null; },
  // 原世界释放第一次仍在途，第二次返回经过游戏核验的结束。
  releaseControl(generation) {
    assert.equal(generation, state.generation);
    return { generation, order: refusedCollection ? 6 : restartRoute || rejoin || (directPlace && !stalePlace) ? 5 :
      first || motion || confirm || pointedCraft || attention || repairPlanObserve || stalePlace ? 4 :
      interrupt || showGame || delayed || departure || (revision && !repairObserve && !repairPlan && !pointedSupport && !observationBatch && !observeDigScope && !continuationBudget) ? 3 : 2,
      phase: ++releases === 1 ? 'requested' : 'released' };
  },
  close() {} } : null);
  try {
    if (controllerRestart) {
      for (let run = 0; run < 2; ++run) {
        if (run) {
          root = fs.mkdtempSync(path.join(temp, 'world-'));
          const nextConfig = path.join(root, 'user', 'plugins', 'mc.companion');
          fs.mkdirSync(nextConfig, { recursive: true });
          fs.copyFileSync(path.join(config, 'settings.json'), path.join(nextConfig, 'settings.json'));
          baselineRead = false;
          collecting = undefined;
          planReading = collectExited = false;
          releases = stopped = 0;
        }
        const child = spawn(exe, [packagePath, root, 'partner-revision-direct-scope'], { stdio: 'inherit', windowsHide: true });
        assert.equal((await once(child, 'exit'))[0], 0);
        assert.equal(stopped, 2);
        assert.equal(releases, 2);
        const trace = path.join(root, 'trace', 'events.jsonl');
        const rows = fs.readFileSync(trace, 'utf8').trim().split('\n').map(JSON.parse);
        const controller = hostWork.at(-1).context.controller;
        const motionExit = rows.find(row => row.category === 'mc.partner.action' && row.body.bridgeWork === collecting &&
          row.body.result.status === 'interrupted');
        const detach = rows.find(row => row.category === 'mc.partner.control' && row.body.controller === controller && row.body.phase === 'released');
        assert.ok(motionExit && detach && motionExit.seq < detach.seq);
        console.log(trace);
      }
      assert.equal(hostWork.length, 2);
      const [first, second] = hostWork;
      assert.notEqual(first.context.controller, second.context.controller);
      assert.deepEqual({ ...second, context: { ...second.context, controller: first.context.controller } }, first);
    } else {
      if (refusedCollection) console.log(path.join(root, 'trace', 'events.jsonl'));
      const child = spawn(exe, [packagePath, root, ...(interrupt || motion || confirm || refusedCollection || task || route || exchange || departure || revision || execution || delivery ? [variant] : [])],
        { stdio: 'inherit', windowsHide: true });
      assert.equal((await once(child, 'exit'))[0], 0);
    }
    if (partnerLoop) assert.equal(releases, execution ? 0 : 2);
    if (showGame) assert.ok(approvedId);
    if (delivery) {
      assert.equal(replies, ['fallback', 'sent', 'finish'].includes(delivery) ? 1 : 0);
      assert.equal(replyReads, delivery === 'unknown' ? 1 : 0);
      const trace = path.join(root, 'trace', 'events.jsonl');
      const rows = fs.readFileSync(trace, 'utf8').trim().split('\n').map(JSON.parse);
      const speech = rows.filter(row => row.category === 'mc.partner.action' && row.body.skill === 'reply').map(row => row.body);
      assert.equal(speech.length, delivery === 'fallback' ? 2 : 1);
      assert.equal(speech[0].bridgeWork, explicitReply.id);
      assert.equal(speech[0].result.delivery, delivery === 'unknown' ? 'unknown' : ['sent', 'finish'].includes(delivery) ? 'sent' : 'not_sent');
      if (delivery === 'fallback') assert.equal(speech[1].source, 'final_text');
      const explicit = rows.find(row => row.kind === 'tool.exited' && row.callId === 'explicit-reply');
      assert.equal(rows.filter(row => row.kind === 'model.prepared' && row.run === explicit.run).length, delivery === 'finish' ? 1 : 2);
      if (delivery === 'finish') {
        assert.ok(rows.some(row => row.kind === 'tool.exited' && row.callId === 'sibling-observe' && row.succeeded));
        assert.ok(rows.some(row => row.category === 'mc.partner.turn' && row.body.closingReply === explicitReply.id && row.body.outcome === 'completed'));
      }
      console.log(trace);
    } else if (questionExpiry) {
      assert.ok(questionPresented && expiredAt !== undefined);
      assert.equal(replies, 1);
      console.log(path.join(root, 'trace', 'events.jsonl'));
    } else if (repairPlanObserve) {
      const trace = path.join(root, 'trace', 'events.jsonl');
      const rows = fs.readFileSync(trace, 'utf8').trim().split('\n').map(JSON.parse);
      const accepted = rows.find(row => row.kind === 'tool.exited' && row.callId === 'accept-observe-plan');
      const { plan, status } = JSON.parse(accepted.output);
      assert.equal(status, 'queued');
      assert.deepEqual(nodeExits, [`plan.${plan}.1`]);
      assert.equal(replies, 1);
      console.log(trace);
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
      assert.equal(alerts[1].body.progress.vertical, 0);
      const turns = rows.filter(row => row.kind === 'model.prepared' && row.step === 1).map(readPartnerContext);
      const blocked = turns.find(turn => turn.input.trigger === 'follow.attention');
      assert.equal(typeof blocked.guidance.domainExperience.followAttention, 'string');
      assert.ok(turns.every(turn => turn.system === blocked.system));
      console.log(trace);
    } else if (continuationBudget) {
      assert.equal(sceneReads.length, 5);
      assert.equal(stopped, 0);
    } else if (openingAnswer) {
      assert.ok(questionPresented && wallFilled);
      assert.equal(stopped, 0);
    } else if (questionScope) {
      assert.ok(questionPresented && collectExited);
      assert.equal(stopped, 0);
    } else if (observeDigScope) {
      assert.ok(collectExited);
      assert.equal(stopped, 0);
      const trace = path.join(root, 'trace', 'events.jsonl');
      const firstTurns = fs.readFileSync(trace, 'utf8').trim().split('\n').map(JSON.parse)
        .filter(row => row.kind === 'model.prepared' && row.step === 1).map(readPartnerContext);
      assert.equal(firstTurns[0].guidance.domainExperience?.nearbyMaterials, undefined);
      const settled = firstTurns.find(turn => ['motion.finished', 'plan.finished'].includes(turn.input.trigger));
      assert.match(settled.guidance.domainExperience.nearbyMaterials, /实际入包/);
      for (const tool of ['observe', 'dig_block', 'gather_ground', 'queue_actions'])
        assert.ok(settled.tools.some(entry => entry.name.endsWith('_' + tool)), tool);
    } else if (layered) {
      assert.equal(nodeExits.length, layeredStepCount);
      assert.equal(replies, layeredStepCount < 4 ? 1 : 0);
      const trace = path.join(root, 'trace', 'events.jsonl');
      const rows = fs.readFileSync(trace, 'utf8').trim().split('\n').map(JSON.parse);
      const paused = checkFollowReceipt(rows, following, following, 'paused');
      const layers = checkPlanReceiptOrder(rows, nodeExits, paused.seq);
      checkFollowReceipt(rows, resumed, following, 'exited', layers.at(-1).seq);
      console.log(trace);
    } else if (responseTiming) {
      assert.equal(timingWorks.length, 2);
      assert.equal(timingSpeech.length, 3);
      const trace = path.join(root, 'trace', 'events.jsonl');
      const rows = fs.readFileSync(trace, 'utf8').trim().split('\n').map(JSON.parse);
      checkResponseTiming(rows, timingWorks, timingSpeech);
      console.log(trace);
    } else if (observationBatch) {
      assert.equal(sceneReads.length, 2);
      const trace = path.join(root, 'trace', 'events.jsonl');
      const rows = fs.readFileSync(trace, 'utf8').trim().split('\n').map(JSON.parse);
      const observations = rows.filter(row => row.kind === 'mail.received' && row.category === 'mc.partner.action' &&
        row.body.skill === 'observe');
      assert.deepEqual(observations.map(row => row.body.bridgeWork), sceneReads);
      const original = rows.find(row => row.kind === 'mail.received' && row.body?.event?.sequence === 2);
      assert.deepEqual(original.body.event.sceneAtMessage, boundaryAnchor);
      for (const entry of observations)
        assert.deepEqual(entry.body.result.observation.samples[0].views[2].anchor, boundaryAnchor);
      const playerTurn = rows.filter(row => row.kind === 'model.prepared' && row.step === 1)
        .map(readPartnerContext).find(turn => turn.input.events.some(event => event.sequence === 2));
      const shown = playerTurn.input.events.find(event => event.sequence === 2).sceneAtMessage;
      assert.deepEqual(shown, { dimension: 'overworld', sampledAtMs: 42, position: { x: 0, y: -60, z: 10 },
        positionCell: { x: -1, y: -61, z: 9 }, facing: { yaw: 0.12, pitch: -0.99 } });
      const queued = rows.find(row => row.kind === 'tool.exited' && row.callId === 'meet-player');
      const next = rows.find(row => row.seq > queued.seq && row.kind === 'model.prepared');
      assert.equal(readPartnerContext(next).input.trigger, 'plan.finished');
      checkPlanReceiptOrder(rows, [meetingWork], 0);
      console.log(trace);
    } else if (revision) {
      if (restartRoute || pointedCraft) {
        const trace = path.join(root, 'trace', 'events.jsonl');
        const rows = fs.readFileSync(trace, 'utf8').trim().split('\n').map(JSON.parse);
        const turns = rows.filter(row => row.kind === 'model.prepared' && row.step === 1).map(readPartnerContext);
        assert.equal(turns[0].input.mcGuidance, undefined);
        assert.ok(turns.every(turn => turn.system === turns[0].system));
        const context = turns.find(turn => restartRoute ? turn.input.trigger === 'task.ready' :
          turn.input.events.some(event => event.sequence === 4));
        const queue = context.tools.find(tool => tool.name.endsWith('_queue_actions'));
        const fields = JSON.parse(queue.parameters).properties;
        assert.match(fields.steps.items.properties.skill.description, /route: Validated route\.id/);
        assert.match(fields.steps.items.properties.skill.description, /craft: Plank item/);
        assert.match(fields.steps.items.properties.arguments.description, /Ordinary sequence parameters:[\s\S]*collect: x,y,z,block,count=1/);
        if (pointedCraft) {
          const original = turns.find(turn => turn.input.events.some(event => event.sequence === 2));
          assert.match(original.guidance.domainExperience.nearbyMaterials, /原话查指向/);
          assert.ok(turns.every(turn => turn.input.trigger !== 'skills.expanded'));
          assert.equal(context.input.pendingQuestion.afterPickup.count, 4);
          assert.match(fields.resolves.description, /saved successor is retained/);
          assert.match(fields.steps.items.properties.pickup.description, /actually enter inventory/);
          assert.ok(pickupPending);
          checkPlanReceiptOrder(rows, noPickup ? [collecting] : [collecting, craftingWork], 0);
        } else {
          assert.ok(restartWork && resumed && restartReads === 9 && routeStops === 2);
          const paused = checkFollowReceipt(rows, following, following, 'paused');
          const { exit } = checkWorkReceipt(rows, routeWork, paused.seq);
          const handoff = context.input.taskHandoff;
          assert.equal(handoff.work, routeWork);
          assert.deepEqual(handoff.action, exit.body);
          const layers = checkPlanReceiptOrder(rows, [restartWork], exit.seq);
          checkFollowReceipt(rows, resumed, following, 'exited', layers.at(-1).seq);
        }
        const settled = turns.find(turn => turn.input.trigger === 'plan.finished' &&
          turn.input.taskPlan?.status !== 'cancelled');
        assert.equal(typeof settled.guidance.outputConstraints.workOutcome, 'string');
        assert.equal(settled.guidance.domainExperience?.nearbyMaterials, undefined);
        assert.deepEqual(settled.tools.map(tool => tool.name.split('_').at(-1)), ['reply']);
      }
      if (correctionLost) assert.ok(stopped >= 2);
      else assert.equal(stopped, notes || restartRoute || pointedSupport || pointedCraft || stalePlace || observeDigScope ? 0 : 2);
      assert.equal(collectExited, !correctionLost && !restartRoute && !stalePlace);
      assert.equal(nextBlock, notes && !directRevision);
      assert.equal(wallFilled, !!correction && !correctionLost);
      if (navigation && !restartRoute) {
        assert.equal(routeReads, mixedRoute ? 10 : 9);
        const rows = fs.readFileSync(path.join(root, 'trace', 'events.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
        const { exit } = checkWorkReceipt(rows, collecting);
        checkPlanReceiptOrder(rows, mixedRoute ? [routeApproach, routeWork, routeDeparture] : [routeWork], exit.seq);
      }
      if (pointedCraft) assert.equal(replies, 3);
      if (directPlace) {
        const rows = fs.readFileSync(path.join(root, 'trace', 'events.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
        const question = rows.find(row => row.kind === 'tool.exited' && row.callId === 'choose-cell');
        const presented = rows.find(row => row.kind === 'mail.received' && row.category === 'mc.partner.question' &&
          row.body.status === 'presented' && row.body.bridgeWork === placementQuestionWork);
        const answer = rows.find(row => row.kind === 'model.prepared' && row.step === 1 &&
          readPartnerContext(row).input.events.some(event => event.sequence === 4));
        assert.deepEqual(JSON.parse(question.output), { status: 'recorded', item: presented.body.item });
        assert.ok(question.seq < presented.seq && presented.seq < answer.seq);
        assert.equal(readPartnerContext(answer).input.questionResult.item, presented.body.item);
        if (stalePlace) assert.equal(collecting, undefined);
        else {
          const { exit } = checkWorkReceipt(rows, collecting, answer.seq);
          assert.equal(exit.body.taskRevision, 1);
          assert.deepEqual(exit.body.result.blocks, [{ ...wall[0], block: 'dirt' }]);
        }
      }
      if (recoverInbox) {
        assert.equal(failedReads, 3);
        assert.ok(workReadFailures > 0);
        const trace = path.join(root, 'trace', 'events.jsonl');
        const rows = fs.readFileSync(trace, 'utf8').trim().split('\n').map(JSON.parse);
        const boundaries = rows.filter(row => row.category === 'mc.partner.feed').map(row => row.body);
        assert.deepEqual(boundaries.map(row => [row.status, row.cursor, row.gap, row.generation]),
          [['recovering', 2, false, state.generation], ['resumed', 3, false, state.generation]]);
        const original = rows.filter(row => row.category === 'mc.partner.event' && row.body.event.sequence === 3);
        assert.equal(original.length, 1);
        assert.equal(original[0].body.event.text, '补记一下角落，接着做');
        assert.ok(rows.some(row => row.category === 'mc.partner.action' && row.body.bridgeWork === collecting &&
          row.body.result.status === 'done' && row.body.result.broken === 2));
      }
      if (rejoin) {
        const trace = fs.readFileSync(path.join(root, 'trace', 'events.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
        const action = trace.findIndex(row => row.category === 'mc.partner.action' && row.body.bridgeWork === collecting &&
          row.body.result.status === 'interrupted' && row.body.result.changes.length === 1);
        const left = trace.findIndex(row => row.category === 'mc.partner.event' && row.body.event.type === 'player.left');
        const entered = trace.findIndex(row => row.category === 'mc.partner.event' && row.body.event.sequence === 4);
        assert.ok(action >= 0 && action < left && left < entered);
      }
      console.log(path.join(root, 'trace', 'events.jsonl'));
    } else if (refusedCollection) assert.equal(collectionWorks.size, 3);
    else if (motion) { assert.equal(replies, 1); assert.equal(stopped, suspend ? 2 : 1); }
    else if (interrupt) assert.equal(stopped, 1);
    else if (partnerLoop) assert.equal(replies, first || delayed || departure || execution ? 0 : 1);
    else if (boardWindow) assert.ok(reads > 0); else assert.equal(reads, 2);
  } finally {
    await bridge.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
