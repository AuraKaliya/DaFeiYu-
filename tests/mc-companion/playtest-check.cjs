const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const { existsSync, readFileSync, writeFileSync } = require('node:fs');
const { join } = require('node:path');
const mineflayer = require('mineflayer');
const { connect, until } = require('./players.cjs');
const { wall, waterCourse, fourCells, layeredStands, readArena } = require('./playtest-arena.cjs');
const { readPartnerContext, scenarioInput } = require('./partner-context.cjs');
const { measurePlayerFeedback } = require('./player-feedback.cjs');

// 读取已经完整写出的证据行，保留仍在写入的末行供下一次轮询。
function readRows(fixture, file) {
  return readFileSync(join(fixture.evidence, 'host', file), 'utf8').split('\n').slice(0, -1).map(JSON.parse);
}

// 通过真实客户端走过一段路，在移动中发出本段的玩家聊天。
// 步骤：转向目标 → 步行并在途中发话 → 到达后松开前进控制。
async function walkCourse(player, point, text) {
  const target = player.entity.position.clone().set(...point);
  const start = player.entity.position.clone();
  await player.lookAt(target.offset(0, player.entity.eyeHeight ?? 1.62, 0), true);
  player.setControlState('forward', true);
  try {
    if (text) {
      await player.waitForTicks(6);
      assert.ok(player.entity.position.distanceTo(start) > 0.25, '发话前玩家已实际走动');
      player.chat(text);
    }
    await until(() => player.entity.position.distanceTo(target) < 0.75, '协议玩家走到路线点 ' + point.join(','));
  } finally { player.setControlState('forward', false); }
}

// 以游戏发言与 Trace 验证同一个跟随工作经过两次聊天、停走及明确退出。
// 步骤：启动跟随并边走边问 → 等待靠近后再起步并聊天 → 对照原工作和退出回执。
async function checkFollow(fixture, player, whispers, course) {
  const received = category => readRows(fixture, 'trace/events.jsonl')
    .filter(row => row.kind === 'mail.received' && row.category === category);
  const motions = () => received('mc.partner.motion');
  const phase = (status, after = 0) => until(() => motions().find(row => row.seq > after && row.body.status === status),
    '跟随进入 ' + status);
  player.chat('你跟着我走');
  const accepted = await phase('accepted');
  await walkCourse(player, course[1], '到哪了');
  await until(() => whispers[0] === '我还在跟着你。', '行走中的显式发言');
  await walkCourse(player, course[2]);
  const paused = await until(() => {
    const latest = motions().at(-1);
    const fish = player.players.DfyCompanion?.entity;
    return latest?.body.status === 'in_range' && fish && fish.position.distanceTo(player.entity.position) <= 3.6 && latest;
  }, '玩家停步后实际进入跟随距离');
  await walkCourse(player, course[3], '继续边走边聊');
  const resumed = await phase('following', paused.seq);
  await until(() => whispers[1] === '我继续跟着你。', '重新追赶时的最终文本发言');

  player.chat('先别跟了');
  const exit = await until(() => received('mc.partner.action').find(row => row.body.skill === 'follow'), '跟随的实际退出');
  const replacement = await until(() => received('mc.partner.action').find(row => row.body.skill === 'move'), '替代移动实际退出');
  await until(() => readRows(fixture, 'trace/events.jsonl').some(row => row.kind === 'tool.exited' &&
    row.callId === 'task_update' && JSON.parse(row.output).task?.next === 'done'), '实际退出后结束局内任务');
  const work = accepted.body.action.bridgeWork;
  const phases = motions();
  const following = phases.filter(row => row.body.action.skill === 'follow');
  assert.equal(following.filter(row => row.body.status === 'accepted').length, 1);
  assert.ok(following.every(row => row.body.action.bridgeWork === work));
  assert.equal(resumed.body.action.bridgeWork, work);
  assert.equal(exit.body.bridgeWork, work);
  assert.equal(exit.body.result.status, 'interrupted');
  assert.equal(exit.body.order, 2);
  const actions = readRows(fixture, 'game-actions.jsonl');
  assert.deepEqual(actions.filter(row => row.skill === 'follow').map(row => [row.work, row.exited, row.result.status]),
    [[work, true, 'interrupted']]);
  const replies = received('mc.partner.action').filter(row => row.body.skill === 'reply');
  assert.deepEqual(replies.map(row => [row.body.order, row.body.result.text]), [[3, whispers[0]], [4, whispers[1]]]);
  assert.ok(replies.every(row => row.seq < exit.seq && actions.some(game => game.work === row.body.bridgeWork && game.result.sent)));
  const trace = readRows(fixture, 'trace/events.jsonl');
  const settings = trace.filter(row => row.kind === 'model.submitted');
  assert.ok(settings.length > 0 && settings.every(row => row.thinking === true && row.model === 'fixed-follow'));
  assert.ok(trace.some(row => row.kind === 'model.prepared' && row.input.messages.some(msg =>
    msg.role === 'assistant' && msg.reasoningContent === 'fixed provider continuation')));
  assert.ok(!whispers.some(text => text.includes('fixed provider continuation')));
  const queue = received('mc.partner.queue');
  assert.deepEqual(queue.map(row => row.body.status), ['queued', 'cleared', 'queued']);
  assert.equal(queue[0].body.works[0], queue[1].body.works[0]);
  assert.ok(!actions.some(row => row.work === queue[0].body.works[0]));
  assert.equal(replacement.body.bridgeWork, queue[2].body.works[0]);
  assert.ok(exit.seq < replacement.seq);
  assert.equal(replacement.body.result.status, 'done');
  const stop = trace.find(row => row.kind === 'tool.exited' && row.callId === 'queue_actions' &&
    JSON.parse(row.arguments).operation === 'replace');
  assert.equal(JSON.parse(stop.arguments).work, work);
  assert.ok(replies[1].seq < stop.seq && stop.seq < exit.seq);
  assert.deepEqual(trace.filter(row => row.kind === 'tool.exited' && row.callId === 'task_update')
    .map(row => JSON.parse(row.output).task.revision), [1, 2]);
}

// 用独立协议玩家走同一真人入口，核对资源包、可见角色和完整聊天回合。
// 步骤：从服务端收到资源包 → 等待收件就绪 → 交叉核对模型输入、游戏动作与真实发言。
async function checkPlayer(fixture, mode) {
  const partner = mode !== '--probe';
  const player = mineflayer.createBot({ host: '127.0.0.1', port: fixture.port, username: 'DfyGuestCheck',
    auth: 'offline', version: '1.21.4' });
  let pack;
  const chat = [];
  const whispers = [];
  player._client.on('add_resource_pack', data => { pack = data; });
  player.on('messagestr', line => { chat.push(line); });
  player.on('whisper', (sender, text) => { if (sender === 'DfyCompanion') whispers.push(text); });
  try {
    await until(() => pack, '真实玩家协议收到体验服资源包', 20000);
    assert.equal(pack.url, 'http://127.0.0.1:25581/companion.zip');
    const response = await fetch(pack.url);
    assert.equal(response.status, 200);
    const digest = createHash('sha1').update(Buffer.from(await response.arrayBuffer())).digest('hex');
    assert.equal(pack.hash, digest);
    await until(() => fixture.listening && (!partner || fixture.settled === 1), '角色就绪并处理入场事实', 45000);
    const fish = await until(() => player.players.DfyCompanion?.entity && player.players.DfyCompanion, '同场可见的独立角色');
    assert.ok(fish.uuid && fish.uuid !== player.player.uuid);
    const actor = fish.uuid;
    const owner = player.player.uuid;
    const doorProbe = mode === '--door-probe';
    const handoff = mode.startsWith('--handoff-');
    const combined = mode === '--follow-dig-fill-probe';
    const resupply = mode === '--layered-resupply-probe';
    const layered = resupply || mode === '--layered-follow-probe';
    const explicitOpenings = mode === '--gap-left-probe' || mode === '--gap-both-probe';
    const gapGroup = explicitOpenings || mode === '--gap-group-probe';
    const wallProbe = doorProbe || mode === '--wall-probe' || mode.startsWith('--ask-') || mode.startsWith('--gap-');
    const vitals = mode === '--vitals-probe' || mode === '--air-episode-probe';
    const arena = partner && !combined && !layered && !gapGroup && !handoff && !vitals && !['--water-probe', '--fill-four-probe', '--gap-materials-probe', '--craft-probe', '--pickup-probe', '--ground-pickup-probe', '--ground-pickup-resume-probe'].includes(mode) ?
      await until(() => readArena(player, wallProbe, doorProbe || mode === '--gap-probe'), '独立客户端读回验收场地') : null;
    if (arena) {
      writeFileSync(join(fixture.evidence, 'arena.json'), JSON.stringify(arena, null, 2));
    }
    if (combined || mode === '--fill-four-probe') {
      await checkFillFour(fixture, player, whispers, combined);
    } else if (mode === '--craft-probe') {
      await checkCraft(fixture, player, whispers);
    } else if (mode === '--ground-pickup-probe' || mode === '--ground-pickup-resume-probe') {
      await checkGroundPickup(fixture, player, whispers,mode === '--ground-pickup-resume-probe');
    } else if (['--handoff-gather-probe','--handoff-moving-probe','--handoff-range-probe','--handoff-lost-probe'].includes(mode)) {
      await checkGatherHandoff(fixture, player, whispers, mode);
    } else if (mode.startsWith('--handoff-eight-')) {
      await checkEightStones(fixture,player,whispers,mode);
    } else if (handoff) {
      const other = mode === '--handoff-other-probe' ? await connect('DfyCollector',fixture.port) : null;
      try { await checkHandoff(fixture, player, whispers, mode, other); }
      finally { other?.quit(); }
    } else if (mode === '--pickup-probe') {
      await checkPickup(fixture, player, whispers);
    } else if (layered) {
      await checkLayeredFollow(fixture, player, whispers, resupply);
    } else if (doorProbe) {
      await checkDoor(fixture, player, whispers);
    } else if (mode === '--water-probe') {
      await checkWater(fixture, player, whispers);
    } else if (vitals) {
      await checkVitals(fixture, player, whispers,mode === '--air-episode-probe');
    } else if (mode === '--gap-materials-probe') {
      await checkMaterials(fixture, player, whispers);
    } else if (explicitOpenings) {
      await checkOpeningScope(fixture, player, whispers, mode === '--gap-left-probe');
    } else if (gapGroup) {
      await checkOpeningGroup(fixture, player, whispers);
    } else if (wallProbe) {
      await checkWall(fixture, player, whispers, mode);
    } else if (mode === '--follow-probe') {
      await checkFollow(fixture, player, whispers, arena.course);
    } else if (partner) {
      assert.deepEqual(whispers, []);
      const settled = seq => until(() => fixture.settled === seq, '处理到玩家事件 ' + seq, 50000);
      player.chat('大肥鱼，你在吗');
      await settled(2);
      const answer = '我在呢，就在你下面不远的地方。看你站在上面一层，我这边是山脚附近，有块小台地可以上去。要不要我上去跟你汇合？';
      await until(() => whispers.length > 0, '仅返回最终文本时的游戏回复', 4000);
      assert.deepEqual(whispers, [answer]);
      player.chat('大肥鱼，帮我采俩泥土');
      await settled(3);
      await until(() => readRows(fixture, 'game-actions.jsonl').some(row => row.skill === 'collect'), '实际采集退出');
      await until(() => {
        const trace = readRows(fixture, 'trace/events.jsonl');
        const followup = trace.find(row => {
          if (row.kind !== 'model.prepared' || scenarioInput(row).messages.at(-1).role !== 'user') return false;
          const work = JSON.parse(scenarioInput(row).messages.at(-1).content).workSettlement;
          return work?.receipts[work.primary]?.skill === 'collect';
        });
        return followup && trace.some(row => row.kind === 'run.exited' && row.session === followup.session && row.run === followup.run);
      }, '原采集回执已交给后续回合');
      player.chat('这回带我往前走走');
      await until(() => fish.entity.position.x > 8, '原移动实际开始');
      player.chat('停，我来');
      await settled(5);
      await until(() => readRows(fixture, 'game-actions.jsonl').some(row => row.skill === 'move' && row.order === 4), '继续原移动直到退出');
      assert.ok(fish.entity.position.x >= 31);
      player.chat('再走回这边');
      await until(() => fish.entity.position.x < 24, '第二次移动实际开始');
      player.chat('停，我来');
      await settled(7);
      await until(() => readRows(fixture, 'game-actions.jsonl').some(row => row.skill === 'move' && row.order === 6), '实际停止第二次移动');
      const stopped = fish.entity.position.clone();
      await player.waitForTicks(12);
      assert.ok(stopped.x > 3 && stopped.distanceTo(fish.entity.position) < 0.5);
      player.chat('模拟一次服务故障');
      await settled(8);
      const receipts = readFileSync(join(fixture.evidence, 'host/game-actions.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
      const inputs = readFileSync(join(fixture.evidence, 'host/model-events.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
      const chosen = inputs.filter(record => record.call);
      assert.deepEqual(receipts.filter(r => !r.work.startsWith('reply.')).map(r => JSON.stringify([r.skill, r.arguments])).sort(),
        chosen.map(r => JSON.stringify([r.call === 'start_move' ? 'move' : r.call, r.arguments])).sort());
      assert.ok(receipts.some(r => r.order === 4 && r.skill === 'move' && r.result.status === 'done'));
      assert.ok(receipts.some(r => r.order === 6 && r.skill === 'move' && r.result.status === 'interrupted'));
      const sent = receipts.filter(r => r.skill === 'reply' && r.result.sent && !r.result.reused);
      assert.deepEqual(sent.map(r => [r.order, r.arguments.text]), [[2, answer], [7, answer]]);
      assert.deepEqual(whispers, [answer, answer]);
    } else {
      player.chat('去山脚看看');
      await until(() => chat.some(line => line.includes('要我采一些带回来吗')), '游戏内提问', 20000);
      player.chat('采 16 块带回来');
      await until(() => chat.some(line => line.includes('我到河边了，这次带回了 16 块圆石')), '游戏内交付', 120000);
    }
    writeFileSync(join(fixture.evidence, 'player.json'), JSON.stringify({ actor, owner, digest, chat }, null, 2));
  } finally { player.quit(); }
}

// 用独立玩家收到的伙伴实体元数据核验水下与回到空气后的只读查询。
// 步骤：固定角色身份和水槽 → 对照掉血与余气采样 → 核实出水恢复及玩家收到的答复。
async function checkVitals(fixture, player, whispers, followDepletion) {
  const actor = player.players.DfyCompanion.entity;
  const keys = player.registry.entitiesByName.player.metadataKeys;
  const air = keys.indexOf('air_supply'), health = keys.indexOf('health');
  const seen = { air: new Set(), health: new Set() };
  const metadata = packet => {
    if (packet.entityId !== actor.id) return;
    for (const row of packet.metadata) {
      if (row.key === air) seen.air.add(row.value);
      if (row.key === health) seen.health.add(row.value);
    }
  };
  player._client.on('entity_metadata',metadata);
  const wet = '我的生命值还没回满，空气已经不到两百四十刻了。';
  const dry = '上岸后我又查了一次，空气已经恢复到三百刻了。';
  const exhausted = '这一次空气确实耗尽了，我连续读到了零和负值。';
  try {
    fixture.send('fill 4 -61 9 6 -57 11 minecraft:glass');
    fixture.send('fill 5 -60 10 5 -58 10 minecraft:water');
    fixture.send('tp DfyCompanion 5.5 -60 10.5');
    await until(() => actor.metadata[air] > 0 && actor.metadata[air] < 210,'旁观玩家核对原角色正在耗气',15000);
    fixture.send('damage DfyCompanion 4 minecraft:generic');
    await until(() => actor.metadata[health] > 0 && actor.metadata[health] < 20,'独立玩家读到角色实际掉血');
    await measurePlayerFeedback(player,'你现在还有多少血和空气？',wet,() => until(() => whispers.includes(wet),'自身生命与余气答复'),
      sample => writeFileSync(join(fixture.evidence,'player-feedback.json'),JSON.stringify(sample,null,2)));
    if (followDepletion) {
      await until(() => actor.metadata[air] < -3,'同一身体的余气连续耗尽',20000);
      await until(() => {
        const rows = readRows(fixture,'trace/events.jsonl');
        const notice = rows.find(r => r.category === 'mc.partner.attention' && r.body.source === 'air' && r.body.trigger === 'vitals.attention');
        return notice && rows.some(r => r.category === 'mc.partner.turn' && r.body.request === notice.body.request && r.body.outcome === 'completed');
      },'耗尽只读回合已复核原角色并静默退出');
      player.chat('刚才真的耗尽空气了吗？');
      await until(() => whispers.includes(exhausted),'按同一次耗尽的连续读数回答');
    }
    fixture.send('tp DfyCompanion 0.5 -60 0.5');
    await until(() => actor.metadata[air] === 300,'独立玩家观察到原角色出水及空气恢复');
    player.chat('上岸后空气恢复了吗？');
    await until(() => whispers.includes(dry),'同一原角色出水后的答复');

    const reads = readRows(fixture,'game-actions.jsonl').filter(row => row.skill === 'observe' && row.result.vitals);
    const orders = followDepletion ? [2,2,3,4] : [2,3];
    assert.deepEqual(reads.map(row => [row.order,row.arguments.mode,row.result.vitals.actor,row.result.vitals.entityId]),
      orders.map(order => [order,'vitals',actor.uuid,actor.id]));
    const under = reads[0].result.vitals, shore = reads.at(-1).result.vitals;
    assert.equal(under.generation,shore.generation);
    assert.ok(under.air.ticks < 240 && under.health.points < 20 && seen.air.has(under.air.ticks) && seen.health.has(under.health.points));
    assert.ok(shore.air.ticks === 300 && seen.air.has(300) && shore.air.sampledAtMs > under.air.sampledAtMs);
    assert.deepEqual([under.shore.status,under.shore.reason,under.shore.airSampledAtMs],
      ['blocked','sealed_volume',under.air.sampledAtMs]);
    assert.deepEqual([shore.shore.status,shore.shore.reason],['unconfirmed','not_in_water']);
    if (followDepletion) {
      const low = reads[2].result.vitals.airEpisode, recovered = shore.airEpisode;
      assert.ok(low.samples >= 2 && low.minimumTicks < 0 && seen.air.has(low.minimumTicks));
      assert.deepEqual([recovered.episode,recovered.phase,recovered.firstAtMs],[low.episode,'recovered',low.firstAtMs]);
      assert.ok(recovered.samples > low.samples && recovered.lastTicks > recovered.recoveryTicks && recovered.recoveryTicks > 0);
      const trace = readRows(fixture,'trace/events.jsonl');
      const prepared = trace.filter(r => r.kind === 'model.prepared' && r.source === 'plugin' && r.step === 1)
        .map(r => ({ input:JSON.parse(scenarioInput(r).messages.at(-1).content), tools:scenarioInput(r).tools }));
      const noticed = prepared.filter(turn => turn.input.airNotices);
      assert.deepEqual(noticed.map(turn => [turn.input.trigger,turn.input.airNotices.map(n => n.airEpisode.phase)]),
        [['vitals.attention',['depleted']],['game.event',['recovered']]]);
      assert.equal(noticed[0].tools.length,1);
      assert.ok(noticed[0].tools[0].name.endsWith('_observe'));
      assert.equal(noticed[1].input.events.at(-1).text,'上岸后空气恢复了吗？');
    }
    assert.deepEqual(whispers,followDepletion ? [wet,exhausted,dry] : [wet,dry]);
    fixture.send('fill 4 -58 9 6 -57 11 minecraft:air');
    fixture.send('setblock 6 -59 10 minecraft:water');
    fixture.send('tp DfyCompanion 5.5 -60 10.5');
    const center = actor.position.clone().set(5,-60,10);
    await until(() => actor.position.floored().equals(center) && player.blockAt(center.offset(0,1,0))?.name === 'water' &&
      player.blockAt(center.offset(1,1,0))?.name === 'water' && actor.metadata[air] > 0 && actor.metadata[air] < 240,
      '独立玩家核对水中正在耗气的伙伴和岸面',15000);
    const before = new Map();
    for (let x=-4; x<=4; ++x) for (let z=-4; z<=4; ++z) for (let y=-2; y<=4; ++y) {
      const p = center.offset(x,y,z);
      before.set(p.toString(),player.blockAt(p)?.stateId);
    }
    player.chat('从水里能上哪边的岸？');
    await until(() => whispers.includes('旁边有能站稳的干岸，我还在水里。'),'收到只读岸边回执');
    const read = readRows(fixture,'game-actions.jsonl').filter(r => r.result.vitals).at(-1).result.vitals;
    const bank = read.shore, stand = center.clone().set(bank.stand.x,bank.stand.y,bank.stand.z);
    assert.deepEqual([read.actor,bank.entityId,bank.airSampledAtMs,bank.reason],[actor.uuid,actor.id,read.air.sampledAtMs,'path_verified']);
    assert.ok(seen.air.has(read.air.ticks) && read.air.ticks < 240 && read.air.sampledAtMs > shore.air.sampledAtMs);
    assert.equal(player.blockAt(stand)?.name,'air');
    assert.equal(player.blockAt(stand.offset(0,-1,0))?.boundingBox,'block');
    assert.ok(actor.position.floored().equals(center));
    assert.deepEqual(fixture.vitalsCommands,[]);
    const cells = [-1,0,1].map(y => {
      const p = stand.offset(0,y,0);
      return { y:p.y, before:before.get(p.toString()), after:player.blockAt(p)?.stateId };
    });
    assert.ok(cells.every(cell => cell.before !== undefined && cell.before === cell.after));
    writeFileSync(join(fixture.evidence,'shore-evidence.json'),JSON.stringify({ actor:actor.uuid, entityId:actor.id,
      air:read.air, shore:bank, cells, blocked:under.shore, dry:shore.shore, commands:fixture.vitalsCommands },null,2));
  } finally {
    player._client.removeListener('entity_metadata',metadata);
    fixture.send('tp DfyCompanion 0.5 -60 0.5');
    fixture.send('fill 4 -60 9 6 -57 11 minecraft:air');
  }
}

// 独立客户端对照原实体、服务端拾取包、旁边的物品和角色的实际入包数量。
// 步骤：区分三堆初始物品 → 用玩家原话委托 → 串查身份、库存、退出与可见回复。
async function checkGroundPickup(fixture, player, whispers, resume) {
  const tracked = (item, count) => Object.values(player.entities).find(entity => {
    const stack = entity.getDroppedItem?.();
    return stack?.name === item && stack.count === count;
  });
  const [original, other, stone] = await until(() => {
    const stacks = [tracked('dirt', 3), tracked('dirt', 2), tracked('stone', 1)];
    return stacks.every(stack => stack?.uuid) && stacks;
  }, '独立客户端识别两堆泥土和一堆石头');
  const actor = player.players.DfyCompanion.entity;
  const packets = [];
  const onCollect = packet => packets.push(packet);
  player._client.on('collect', onCollect);
  try {
    assert.equal(fixture.materialCount('dirt'), 0);
    const message = '把那一堆三块泥土捡起来。', reply = '三块泥土都进了我的背包。';
    await measurePlayerFeedback(player, message, reply, () => until(() => whispers.includes(reply), '实际入包后的玩家回复', 18000),
      sample => writeFileSync(join(fixture.evidence, 'player-feedback.json'), JSON.stringify(sample, null, 2)));
    const actions = readRows(fixture, 'game-actions.jsonl');
    const jobs = actions.filter(row => row.skill === 'pickup_drop');
    assert.equal(jobs.length, 1);
    const [job] = jobs;
    const source = actions.find(row => row.work === job.arguments.observation);
    assert.ok(source?.result.dropObservation.entities.some(entity => entity.uuid === original.uuid && entity.count === 3));
    assert.equal(job.order, source.order);
    assert.deepEqual([job.arguments.entity, job.result.pickup.observed.uuid, job.result.pickup.observed.lastCollector.uuid],
      [original.uuid, original.uuid, actor.uuid]);
    assert.deepEqual([job.exited, job.result.status, job.result.pickup.verifiedCount, job.result.inventoryDelta],
      [true, 'done', 3, { dirt: 3 }]);
    assert.deepEqual(packets.filter(packet => packet.collectedEntityId === original.id).map(packet =>
      [packet.collectorEntityId, packet.pickupItemCount]), [[actor.id, 3]]);
    assert.equal(tracked('dirt', 2)?.uuid, other.uuid);
    assert.equal(tracked('stone', 1)?.uuid, stone.uuid);
    assert.equal(fixture.materialCount('dirt'), 3);
    assert.deepEqual(whispers, [reply]);
    assert.equal(actions.filter(row => row.skill === 'reply' && row.result.text === reply && row.result.sent).length, 1);
    assert.ok(readRows(fixture, 'trace/events.jsonl').some(row => row.category === 'mc.partner.action' &&
      row.body.bridgeWork === job.work && row.body.order === job.order && row.body.result?.pickup?.verifiedCount === 3));

    fixture.send(`tp ${stone.uuid} 0.85 -60 -5.5`);
    await until(() => stone.position.distanceTo(other.position) < 1, '两堆不同材质在客户端实际重叠');
    const start = actor.position.clone();
    const stopped = '那两块泥土紧挨着石头，一起捡起的风险太大，我停在原地了。';
    player.chat('只拿剩下的两块泥土，石头留给我。');
    await until(() => whispers.includes(stopped), '直接工具读回相邻掉落并回答玩家', 15000);
    const attempts = readRows(fixture, 'game-actions.jsonl').filter(row => row.skill === 'pickup_drop');
    assert.equal(attempts.length, 2);
    const blocked = attempts[1];
    assert.deepEqual([blocked.arguments.entity, blocked.result.nearbyDrop.uuid, blocked.result.pickup.approachAttempted,
      blocked.result.reasonCode, blocked.result.inventoryDelta], [other.uuid, stone.uuid, false, 'pickup_other_stack_nearby', {}]);
    assert.ok(actor.position.distanceTo(start) < 0.1);
    assert.equal(fixture.materialCount('dirt'), 3);
    assert.deepEqual(whispers, [reply, stopped]);
    if (resume) await checkGroundResume(fixture,player,whispers,actor,other,stone,blocked,packets);
  } finally { player._client.removeListener('collect', onCollect); }
}

// 原堆受阻后只向伙伴提供现场变化，沿原 UUID 复查并续取，独立玩家核对旁堆与服务端收取包。
// 步骤：换成同物同数旁堆 → 验证仍停步 → 移开旁堆并核对原目标实收 → 重复查询已退出工作。
async function checkGroundResume(fixture, player, whispers, actor, target, stone, blocked, packets) {
  const actions = () => readRows(fixture,'game-actions.jsonl');
  const before = fixture.materialCount('dirt');
  fixture.send('tp '+stone.uuid+' -5.5 -60 -4.5');
  fixture.send('summon minecraft:item 0.85 -59.9 -5.5 {PickupDelay:32767s,Age:-32768s,Item:{id:"minecraft:dirt",count:2},Motion:[0d,0d,0d]}');
  const decoy = await until(() => Object.values(player.entities).find(entity => entity.uuid !== target.uuid && entity.getDroppedItem?.()?.name === 'dirt' &&
    entity.getDroppedItem().count === 2 && entity.position.distanceTo(target.position) < 1),'旁边同物同数保有独立 UUID');
  await until(() => fixture.groundEntity(stone.uuid)?.position.distanceTo(target.position) > 4 &&
    fixture.groundEntity(decoy.uuid)?.getDroppedItem?.()?.count === 2,'伙伴实际看见石头已移开和同物旁堆');
  player.chat('石头挪开了。');
  await until(() => whispers.includes('原堆旁边还有另一堆泥土，我先停在原地。'),'收到条件更新后仍只认原堆');
  const guarded = actions().find(row => row.skill === 'pickup_drop' && row.arguments.resumeWork === blocked.work);
  assert.deepEqual([guarded.exited,guarded.result.reasonCode,guarded.result.nearbyDrop.uuid,guarded.result.pickup.verifiedCount,
    guarded.arguments.entity,guarded.arguments.observation],[true,'pickup_other_stack_nearby',decoy.uuid,0,target.uuid,blocked.arguments.observation]);
  assert.equal(fixture.materialCount('dirt'),before);
  fixture.send('tp '+decoy.uuid+' -7.5 -60 0.5');
  await until(() => decoy.position.distanceTo(target.position) > 5 && fixture.groundEntity(decoy.uuid)?.position.distanceTo(target.position) > 5,
    '独立玩家和伙伴均看见旁堆已移开且原目标仍在');
  player.chat('旁边那堆也挪开了。');
  await until(() => whispers.includes('原来那堆剩下的两块已进背包。'),'沿已退出原工作自动续取并向玩家核实',20000);
  const completed = actions().find(row => row.skill === 'pickup_drop' && row.arguments.resumeWork === guarded.work);
  const proof = completed.result.pickup;
  assert.deepEqual([completed.exited,completed.arguments.entity,proof.continuation.root,proof.continuation.sourceMessage,proof.verifiedCount,
    proof.observed.lastCollector.uuid,fixture.materialCount('dirt')],[true,target.uuid,blocked.work,blocked.order,2,actor.uuid,before+2]);
  assert.deepEqual(packets.filter(row => row.collectedEntityId === target.id).map(row => [row.collectorEntityId,row.pickupItemCount]),[[actor.id,2]]);
  assert.equal(packets.some(row => row.collectedEntityId === decoy.id),false);
  const again = 'duplicate-ground-resume';
  await fixture.pickupCall('partner.begin',completed,again);
  const refused = await until(() => actions().find(row => row.work === again),'换号续取回指已完成工作');
  assert.deepEqual([refused.result.originalWork,refused.result.pickup],[completed.work,undefined]);
  await fixture.pickupCall('partner.begin',completed,'completed-ground-resume',{...completed.arguments,resumeWork:completed.work});
  const exhausted = await until(() => actions().find(row => row.work === 'completed-ground-resume'),'已完成的原工作没有可续取余量');
  assert.deepEqual([exhausted.result.reasonCode,exhausted.result.originalWork,exhausted.result.pickup],['pickup_already_completed',completed.work,undefined]);
  for (const old of [blocked,guarded,completed]) {
    const read = (await fixture.pickupCall('partner.receipt',old,old.work)).action;
    assert.deepEqual([read.exited,read.result.pickup.verifiedCount],[true,old.result.pickup.verifiedCount]);
  }
  for (let i=0; i<2; ++i) {
    const start = whispers.length;
    player.chat('再核对原来的掉落。');
    await until(() => whispers.slice(start).includes('原堆已离开视野，按原工作核对过了。'),'复查已退出原目标不再接近或拾取');
  }
  const reads = actions().filter(row => row.skill === 'observe' && row.result.dropTarget?.entity === target.uuid);
  assert.deepEqual(reads.slice(-2).map(row => row.result.dropTarget.state),['not_tracked','not_tracked']);
  assert.deepEqual([decoy.getDroppedItem().count,fixture.materialCount('dirt'),actions().filter(row =>
    row.skill === 'pickup_drop' && row.result.pickup && row.arguments.entity === target.uuid).length],[2,before+2,3]);
  const decisions = [];
  if (fixture.factPilot) {
    const trace = readRows(fixture,'trace/events.jsonl');
    for (const row of trace.filter(entry => entry.kind === 'tool.exited' && entry.callId === 'read-ground-effect')) {
      const read = JSON.parse(row.output);
      const item = read.status.items.find(sample => sample.key === 'mc.companion.pickup_effect');
      const requested = JSON.parse(row.arguments).snapshot;
      const prepared = trace.find(entry => entry.kind === 'model.prepared' && entry.session === row.session &&
        entry.run === row.run && entry.step === row.step);
      assert.equal(read.state,'ready');
      assert.equal(read.status.snapshot,requested);
      assert.equal(JSON.parse(prepared.input.messages.at(-1).content).snapshot,requested);
      assert.ok(item.fact > 0 && item.subject && item.value.entity === target.uuid);
      decisions.push({snapshot:requested,work:item.subject,fact:item.fact,acquired:item.value.acquired,
        pending:item.value.pendingPickup,remainingState:item.value.remainingState});
    }
    assert.deepEqual(decisions.map(row => [row.acquired,row.pending,row.remainingState]),
      [[0,2,'observed'],[0,2,'observed'],[2,0,'accounted']]);
    assert.deepEqual(decisions.map(row => row.work),[blocked.work,guarded.work,completed.work]);
    assert.equal(new Set(decisions.map(row => row.snapshot)).size,3);
    assert.equal(new Set(decisions.map(row => row.fact)).size,3);
  }
  writeFileSync(join(fixture.evidence,'ground-resume-evidence.json'),JSON.stringify({entity:target.uuid,neighbor:decoy.uuid,
    observation:blocked.arguments.observation,sourceMessage:blocked.order,blocked:blocked.work,guarded:guarded.work,completed:completed.work,
    continuation:proof.continuation,before,after:fixture.materialCount('dirt'),received:proof.verifiedCount,neighborRemaining:decoy.getDroppedItem().count,
    duplicate:refused.result.originalWork,reads:reads.map(row => ({work:row.work,state:row.result.dropTarget.state})),
    ...(fixture.factPilot ? {decisions} : {})},null,2));
}

// 从零库存核验原任务的两次破坏、入包、会合和玩家实收，并在采集中询问实际进度。
// 步骤：记录原话和首条有用反馈 → 读取在途工作回答插话 → 对照双方库存、逐格变化及完整工序。
async function checkGatherHandoff(fixture, player, whispers, mode) {
  const moving = mode !== '--handoff-gather-probe';
  const cells = [[-2,-60,0],[-3,-60,0]].map(c => player.entity.position.clone().set(...c));
  const dirt = player.registry.itemsByName.dirt.id;
  await until(() => fixture.materialCount('dirt') === 0 && player.inventory.items().length === 0 &&
    cells.every(cell => player.blockAt(cell)?.name === 'grass_block'), '两处取材格、伙伴零泥土与收件玩家空背包');
  const first = '我背包里还没有泥土，正在附近给你备两块。', during = '我在挖第一块，泥土还没进背包。';
  const finished = '刚挖的两块泥土你都捡到了。';
  const trace = () => readRows(fixture,'trace/events.jsonl');
  await measurePlayerFeedback(player,'就地备两块泥土，拿来给我。',first,() => until(() => whispers.includes(first),'核实零库存后的首条答复'),
    sample => writeFileSync(join(fixture.evidence,'player-feedback.json'),JSON.stringify(sample,null,2)));
  const digging = await until(() => trace().find(r => r.category === 'mc.partner.motion' && r.body.status === 'digging'), '原任务已开挖');
  await measurePlayerFeedback(player,'备好几块了？',during,() => until(() => whispers.includes(during),'采掘插话回答'),
    sample => writeFileSync(join(fixture.evidence,'progress-feedback.json'),JSON.stringify({ ...sample, work: digging.body.action.bridgeWork },null,2)));
  if (moving) {
    const ranged = mode === '--handoff-range-probe', lost = mode === '--handoff-lost-probe';
    const firstX = ranged ? 34.5 : 10.5;
    fixture.send('tp DfyGuestCheck '+firstX+' -60 0.5');
    await until(() => player.entity.position.x === firstX, '配对玩家已离开原交物位置');
    await until(() => {
      const oldGive = readRows(fixture,'game-actions.jsonl').find(row => row.skill === 'give_item');
      if (oldGive) throw new Error('伙伴在旧站位尝试交物，回执 '+oldGive.result.reasonCode+'，已提交 '+oldGive.result.transfer?.submitted);
      return player.players.DfyCompanion.entity.position.x >= (ranged ? 30.5 : 3.5);
    },'伙伴携两块材料向配对玩家的新位置移动',45000);
    assert.deepEqual([fixture.materialCount('dirt'),player.inventory.count(dirt)],[2,0]);
    if (ranged || lost) {
      fixture.send('tp DfyGuestCheck 72.5 -60 0.5');
      const stopped = ranged ? '我已经备好两块泥土。你离开了这次会合的范围，两块都还在我这里。' :
        '两块泥土备好了。我现在看不到你的位置，两块都还在我这里。';
      await until(() => whispers.includes(stopped),'超出原范围的会合实际停止并向原玩家说明',15000);
      const actions = readRows(fixture,'game-actions.jsonl');
      const meet = actions.find(row => row.skill === 'move');
      assert.deepEqual([meet.exited,meet.result.status,meet.result.reasonCode,actions.some(row => row.skill === 'give_item')],
        [true,'blocked',ranged ? 'meeting_out_of_range' : 'meeting_player_unavailable',false]);
      assert.deepEqual([fixture.materialCount('dirt'),player.inventory.count(dirt),whispers],[2,0,[first,during,stopped]]);
      return;
    }
    fixture.send('tp DfyGuestCheck 7.5 -60 8.5');
    await until(() => player.entity.position.z === 8.5, '玩家在会合途中再次移动');
  }
  await until(() => whispers.includes(finished),'两块实际入包并交给原玩家',45000);

  const actions = readRows(fixture,'game-actions.jsonl');
  const jobs = actions.filter(row => ['collect','move','give_item'].includes(row.skill));
  // 桥接按最新在场原话准入。计划和宿主动作保留委托来源，二者通过同一工作号关联。
  assert.deepEqual(jobs.map(row => [row.skill,row.order,row.exited,row.result.status]), [['collect',2,true,'done'],['move',3,true,'done'],['give_item',3,true,'done']]);
  const [soil,meet,give] = jobs;
  assert.equal(soil.work,digging.body.action.bridgeWork);
  assert.deepEqual([soil.result.broken,soil.result.stock.before,soil.result.stock.after,soil.result.pickups.selfCollected.dirt],[2,0,2,2]);
  assert.equal(meet.result.arrived,true);
  assert.deepEqual([meet.arguments.place,meet.arguments.recipient,meet.result.meeting.recipient,meet.result.meeting.entityId],
    ['player',player.player.uuid,player.player.uuid,player.entity.id]);
  if (moving) {
    const end = player.entity.position.clone().set(meet.result.position.x,meet.result.position.y,meet.result.position.z);
    assert.ok(end.distanceTo(player.entity.position) <= 3 && end.distanceTo(end.clone().set(10.5,-60,0.5)) > 3);
    assert.equal(give.result.handoff.target.entityId,player.entity.id);
  }
  assert.deepEqual([give.result.transfer.stockBefore,give.result.transfer.stockAfter,give.result.receipt.recipientPicked],[2,0,2]);
  assert.ok(cells.every(cell => player.blockAt(cell)?.name === 'air'));
  assert.deepEqual([fixture.materialCount('dirt'),player.inventory.count(dirt)], [0,2]);
  assert.deepEqual(whispers, [first,during,finished]);
  const accepted = trace().filter(r => r.kind === 'mail.received' && r.category === 'mc.partner.action' && jobs.some(job => job.work === r.body.bridgeWork));
  assert.deepEqual(accepted.map(r => [r.body.bridgeWork,r.body.order]),jobs.map(job => [job.work,2]));
  const plan = trace().find(r => r.category === 'mc.partner.plan' && r.body.status === 'queued')?.body.plan;
  assert.deepEqual([plan.origin.sequence,plan.origin.text,plan.nodes.map(n => n.work)], [2,'就地备两块泥土，拿来给我。',jobs.map(job => job.work)]);
  const delivery = await until(() => readRows(fixture,'game-actions.jsonl').find(r => r.skill === 'reply' && r.result.sent && r.arguments.text === finished),
    '玩家收件对应的原发言工作已退出');
  const replies = await until(() => {
    const facts = trace().filter(r => r.kind === 'mail.received' && r.category === 'mc.partner.action' && r.body.skill === 'reply').map(r => r.body);
    return facts.some(fact => fact.bridgeWork === delivery.work) && facts;
  },'按原发言工作号等宿主归档收件回执');
  assert.deepEqual(replies.map(r => [r.result.text,r.order,r.replyOrigin.sourceMessage]),[[first,2,2],[during,3,3],[finished,3,2]]);
  assert.deepEqual(replies[2].replyOrigin,{ sourceMessage:2, planId:plan.id, taskRevision:plan.revision });
  assert.equal(replies[2].source,'final_text');
  assert.ok(replies[0].invocation > 0);
  player.chat('刚才交的是我哪次要的？');
  await until(() => whispers.includes('是你让我就地备两块泥土的那次。'),'下轮可沿完工发言找到原委托');
  if (fixture.factPilot) {
    const frames = trace().filter(r => r.kind === 'model.prepared' && scenarioInput(r).messages.at(-1).role === 'user')
      .map(r => JSON.parse(scenarioInput(r).messages.at(-1).content));
    assert.ok(frames.some(f => f.factFrame?.mode === 'baseline') && frames.some(f => f.factFrame?.mode === 'delta'));
    assert.ok(frames.some(f => f.hostWorkReceipts?.some(w => w.work === give.work && w.exited)));
    for (const frame of frames.filter(f => f.hostWorkReceipts?.length))
      assert.ok(frame.hostWorkReceipts.every(w => w.receipt > frame.factFrame.workAfter && w.receipt <= frame.factFrame.workThrough));
    assert.ok(!trace().some(r => r.kind === 'mail.received' && r.body?.factReceiptError));
  }
}

// 通过原版协议核对八块圆石的实际破坏、入包及只约束交付的选箱问题。
// 步骤：下达原话并在开挖时插问 → 核对八个格和库存 → 按选箱或满包投物分流 → 验证原号实收。
async function checkEightStones(fixture, player, whispers, mode) {
  const id = player.registry.itemsByName.cobblestone.id;
  const point = (x,y,z) => player.entity.position.clone().set(x,y,z);
  const cells = [-3,-2].flatMap(x => [-2,-1,0,1].map(z => point(x,-60,z)));
  const actions = () => readRows(fixture,'game-actions.jsonl');
  const trace = () => readRows(fixture,'trace/events.jsonl');
  await until(() => cells.every(p => player.blockAt(p)?.name === 'cobblestone') &&
    [point(2,-60,-2),point(2,-60,2)].every(p => player.blockAt(p)?.name === 'chest') &&
    fixture.materialCount('cobblestone') === 0,'八格目标、两个箱子与伙伴零库存');
  const question = '八块圆石交到哪个箱子？';
  await measurePlayerFeedback(player,'帮我挖 8 块圆石',question,() => until(() => whispers.includes(question),'玩家实际收到交付追问'),
    sample => writeFileSync(join(fixture.evidence,'player-feedback.json'),JSON.stringify(sample,null,2)));
  await until(() => trace().some(r => r.category === 'mc.partner.motion' && r.body.status === 'digging'),'选箱待答时采掘已开始',20000);
  let progress;
  await measurePlayerFeedback(player,'挖进背包几块了？',() => progress,async () => {
    const answer = await until(() => whispers.find(line => /这次查到我背包里有 \d+ 块圆石/.test(line)),'采掘中的实测库存答复');
    const stock = actions().find(row => row.result.handoff)?.result.handoff.stock.available;
    assert.ok(Number.isInteger(stock) && stock <= 8 && answer === '这次查到我背包里有 '+stock+' 块圆石。');
    progress = answer;
  },sample => writeFileSync(join(fixture.evidence,'progress-feedback.json'),JSON.stringify(sample,null,2)));
  await until(() => whispers.includes('八块圆石都进了我的背包，等你选交付位置。'),'原采掘工作核实入包并结算',60000);
  const dig = actions().filter(row => row.skill === 'collect');
  assert.deepEqual([dig.length,dig[0].result.broken,dig[0].result.inventoryDelta.cobblestone,fixture.materialCount('cobblestone')],[1,8,8,8]);
  assert.ok(cells.every(p => player.blockAt(p)?.name === 'air') && !actions().some(row => row.skill === 'give_item'));
  player.chat('可以先交了吗？');
  await until(() => whispers.includes('还在等你选交付位置，八块都在我这里。'),'实际给物候选被同一待答阻止');
  assert.ok(!actions().some(row => row.skill === 'give_item'));
  if (['--handoff-eight-chest-probe','--handoff-eight-store-probe','--handoff-eight-withdraw-probe','--handoff-eight-withdraw-boundary-probe','--handoff-eight-chest-delivery-probe'].includes(mode)) {
    fixture.send('tp DfyCompanion 0.5 -60 0.5');
    await until(() => player.players.DfyCompanion.entity.position.distanceTo(point(0.5,-60,0.5)) < 0.05,'伙伴回到可读箱位置');
    // 用独立协议玩家分别打开两个真实箱子，逐次关闭原窗口。
    const contents = async () => {
      const counts = [];
      for (const z of [-2,2]) {
        const box = await player.openContainer(player.blockAt(point(2,-60,z)));
        try { counts.push(box.countRange(0,box.inventoryStart,id)); } finally { box.close(); }
      }
      return counts;
    };
    assert.deepEqual(await contents(),[0,5]);
    await player.lookAt(point(2.5,-59.5,-1.5),true);
    await until(() => Math.cos(fixture.ownerEntity()?.yaw) > 0.99 && fixture.ownerEntity()?.pitch < -0.3,'伙伴观察到玩家指向左箱');
    player.chat('左箱');
    await until(() => whispers.includes('左箱能再放 1728 块圆石，我背包里有 8 块。'),'读回指定单箱及伙伴库存');
    const read = actions().find(row => row.result.chest);
    assert.deepEqual([read.result.chest.owner,read.result.chest.generation,read.result.chest.dimension,
      read.result.chest.cell,read.result.chest.container.slots,read.result.chest.stock.slots,read.result.chest.stock.stored],
      [player.player.uuid,read.generation,'overworld',{x:2,y:-60,z:-2},27,36,8]);
    assert.deepEqual([await contents(),fixture.materialCount('cobblestone'),player.inventory.count(id)],[[0,5],8,0]);
    if (mode === '--handoff-eight-chest-delivery-probe') await require('./chest-delivery-check.cjs').checkChestDelivery(fixture,player,whispers,contents,actions,trace);
    if (mode === '--handoff-eight-withdraw-probe' || mode === '--handoff-eight-withdraw-boundary-probe')
      await require('./chest-withdrawal-check.cjs').checkChestWithdrawals(fixture,player,whispers,contents,actions,trace,mode === '--handoff-eight-withdraw-boundary-probe');
    if (mode === '--handoff-eight-store-probe') await checkChestDeposits(fixture,player,whispers,contents,actions,trace);
    return;
  }
  fixture.send('tp DfyCompanion 0.5 -60 0.5');
  fixture.send('tp DfyGuestCheck 2.5 -60 0.5');
  fixture.send('give DfyGuestCheck minecraft:stone 2304');
  await until(() => player.inventory.emptySlotCount() === 0 && player.entity.position.distanceTo(point(2.5,-60,0.5)) < 0.05 &&
    player.players.DfyCompanion.entity.position.distanceTo(point(0.5,-60,0.5)) < 0.05,'收件玩家在平坦落点前两格且背包已满');
  player.chat('扔给我');
  await until(() => whispers.includes('八块都已投出，你的拾取还没有确认。'),'满包阶段只报告原投物实效',20000);
  const give = actions().filter(row => row.skill === 'give_item');
  const proof = give[0].result.receipt;
  assert.deepEqual([give.length,proof.thrown,proof.recipientPicked,proof.otherPicked,proof.pickupUnknown,player.inventory.count(id)],[1,8,0,0,8,0]);
  if (mode === '--handoff-eight-retired-probe') {
    await checkRetiredPickup(fixture,player,give[0],id);
    return;
  }
  fixture.send('clear DfyGuestCheck minecraft:stone');
  await until(() => player.inventory.count(id) === 8,'腾包后玩家实际收到八块');
  player.chat('现在收到几块？');
  await until(() => whispers.includes('查回原交付，你实收了八块圆石。'),'原工作迟到实收答复');
  const check = actions().find(row => row.result.handoffReceipt)?.result.handoffReceipt;
  assert.deepEqual([check.work,check.receipt.recipientPicked,actions().filter(row => row.skill === 'give_item').length],[give[0].work,8,1]);
  player.chat('再核对一次原投递号');
  await until(() => whispers.includes('原投递号已核对，实收仍是八块。'),'已退出的原工作获得迟到实效');
  const frames = trace().filter(row => row.kind === 'model.prepared' && scenarioInput(row).messages.at(-1).role === 'user')
    .map(row => JSON.parse(scenarioInput(row).messages.at(-1).content));
  const original = frames.flatMap(frame => frame.hostWorkReceipts ?? []).find(work => work.work === give[0].work && work.revision === 4);
  assert.deepEqual([original.source.skill,original.effect.recipient,original.effect.recipientPicked,original.effect.sourceThrough],
    ['give_item',player.player.uuid,8,proof.throughSeq]);
  assert.ok(original.exited && !trace().some(row => row.body?.factReceiptError));
  assert.equal(actions().filter(row => row.skill === 'give_item').length,1);
}

// 在同一隔离双箱现场按新委托验证完整、满箱、部分与窗口中断的真实库存。
// 步骤：备好各局容量并指箱 → 记录原工作 → 玩家独立数双箱 → 按原号重复查实效。
async function checkChestDeposits(fixture, player, whispers, contents, actions, trace) {
  const away = player.entity.position.clone().set(2.5,-54,0.5);
  const rows = () => actions().filter(row => row.skill === 'store_item' && row.result.deposit);
  const evidence = [];
  // 由协议玩家分别数两个真实容器，再取伙伴自身已同步的普通圆石数量。
  const sample = async () => {
    const [left,right] = await contents();
    return {left,right,companion:fixture.materialCount('cobblestone'),sampledAtMs:Math.floor(performance.now())};
  };
  for (const [index,scenario] of ['normal','full','partial','interrupted'].entries()) {
    const initial = scenario === 'partial' ? 61 : 0;
    if (index) {
      const entries = ['partial','full'].includes(scenario) ? Array.from({length:27},(_,slot) =>
        '{Slot:'+slot+'b,id:"minecraft:'+(initial && slot === 0 ? 'cobblestone' : 'stone')+'",count:'+(initial && slot === 0 ? 61 : 64)+'}') : [];
      fixture.send('data modify block 2 -60 -2 Items set value ['+entries.join(',')+']');
      fixture.send('clear DfyCompanion minecraft:cobblestone');
      fixture.send('give DfyCompanion minecraft:cobblestone 8');
      await until(() => fixture.materialCount('cobblestone') === 8,'伙伴收到本局八块圆石');
    }
    const before = await sample();
    assert.deepEqual([before.left,before.right,before.companion],[initial,5,8]);
    await player.lookAt(away,true);
    await until(() => fixture.ownerEntity()?.pitch > 1,'玩家已从所选左箱转头看向上方');
    if (scenario === 'interrupted') fixture.interruptChest();
    const start = whispers.length;
    player.chat(index % 2 ? '请把8块圆石放入左边的箱子' : '把这八块存进左箱');
    const row = await until(() => rows()[index],'存箱原工作实际退出',20000);
    const stored = scenario === 'full' ? 0 : scenario === 'partial' ? 3 : 8;
    const report = '左箱实存 '+stored+' 块，剩余 '+(8-stored)+' 块。';
    const interrupted = scenario === 'interrupted';
    const proof = row.result.deposit;
    const selection = actions().find(entry => entry.result.chest);
    assert.deepEqual([proof.observation,proof.pointedMessage],[selection.work,selection.order]);
    assert.deepEqual([proof.work,proof.sourceMessage,proof.state,proof.stored,proof.remaining],
      [row.work,row.order,interrupted ? 'pending' : 'verified',interrupted ? undefined : stored,interrupted ? undefined : 8-stored]);
    await until(() => whispers.slice(start).includes(interrupted ? '存箱工作 '+row.work+' 的实存量待核实。' : report),'按实效回答本局玩家');
    const after = await sample();
    assert.deepEqual([after.left,after.right,after.companion],[initial+stored,5,8-stored]);
    assert.equal((await fixture.repeatChest(row,row.work)).action.exited,true);
    const duplicateId = 'repeat-store-'+index;
    await fixture.repeatChest(row,duplicateId);
    const duplicate = await until(() => actions().find(entry => entry.work === duplicateId),'同一玩家原话换工作号仍指回原件');
    assert.equal(duplicate.result.originalWork,row.work);
    assert.equal(duplicate.result.deposit,undefined);
    let recheck;
    if (interrupted) {
      const replyStart = whispers.length;
      player.chat('复查存箱工作 '+row.work);
      await until(() => whispers.slice(replyStart).includes(report),'沿原号重新开箱核对中断后的数量');
      const checked = actions().find(entry => entry.result.chestReceipt?.work === row.work);
      assert.deepEqual([checked.result.chestReceipt.stored,checked.result.chestReceipt.remaining],[8,0]);
      const settled = await until(() => trace().find(entry => entry.kind === 'binding.work_settled' && entry.work === row.work &&
        entry.revision === 4 && entry.finalEffect && entry.body.stored === 8),'迟到双边实效回到原工作');
      recheck = {query:checked.work,original:settled.work,revision:settled.revision,stored:checked.result.chestReceipt.stored,
        remaining:checked.result.chestReceipt.remaining};
    }
    assert.deepEqual([rows().length,await contents(),fixture.materialCount('cobblestone')],[index+1,[initial+stored,5],8-stored]);
    evidence.push({scenario,world:proof.generation,dimension:proof.dimension,selected:proof.cell,other:{x:2,y:-60,z:2},
      observation:proof.observation,pointedMessage:proof.pointedMessage,sourceMessage:proof.sourceMessage,work:row.work,
      status:proof.state,stored:proof.stored,remaining:proof.remaining,rejectedDuplicate:duplicate.result.originalWork,before,after,recheck});
    writeFileSync(join(fixture.evidence,'chest-evidence.json'),JSON.stringify(evidence,null,2));
  }
}

// 让原玩家在投物后离场，验证重新入场后的真实拾取只结算到原绑定。
// 步骤：确认旧工作接纳及离场 → 用同一玩家重新连接并腾包 → 串查独立库存、原回执与新会话输入。
async function checkRetiredPickup(fixture, player, give, item) {
  const trace = () => readRows(fixture,'trace/events.jsonl');
  const old = trace().find(row => row.kind === 'binding.work_settled' && row.work === give.work && row.phase === 'accepted');
  assert.ok(old);
  fixture.rejoining = true;
  player.quit();
  let joined;
  try {
    const left = await until(() => trace().find(row => row.category === 'mc.partner.event' && row.body.event.type === 'player.left'),
      '原配对离场已处理');
    joined = await connect('DfyGuestCheck',fixture.port);
    const next = await until(() => trace().find(row => row.seq > left.seq && row.kind === 'model.prepared' &&
      row.source === 'plugin' && row.session !== old.session),'重新入场后建立新会话');
    assert.ok(!JSON.parse(scenarioInput(next).messages.at(-1).content).hostWorkReceipts?.some(entry => entry.work === give.work));
    fixture.send('clear DfyGuestCheck minecraft:stone');
    await until(() => joined.inventory.count(item) === 8,'新连接独立读到八块圆石');
    const settled = await until(() => trace().find(row => row.kind === 'binding.work_settled' && row.work === give.work &&
      row.finalEffect && row.body.recipientPicked === 8),'旧绑定收到迟到实收',15000);
    assert.deepEqual([settled.session,settled.epoch,settled.retired,settled.body.recipient,settled.body.sourceThrough],
      [old.session,old.epoch,true,joined.player.uuid,give.result.receipt.throughSeq]);
    assert.equal(readRows(fixture,'game-actions.jsonl').filter(row => row.skill === 'give_item').length,1);
    assert.equal(trace().filter(row => row.kind === 'binding.work_settled' && row.work === give.work && row.finalEffect).length,1);
  } finally { fixture.rejoining = false; joined?.quit(); }
}

// 在两个真实投物操作之间移开玩家，核对停投读数、现场掉落及原委托的实际发言。
// 步骤：等待第一堆入世界 → 移出交物距离 → 对照原工作、玩家端物品及两边库存。
async function checkPartialHandoff(fixture, player, whispers, initialReply) {
  const dirt = player.registry.itemsByName.dirt.id;
  const origin = player.entity.position.clone();
  const ground = () => Object.values(player.entities).filter(entity => entity.position.distanceTo(origin) <= 5)
    .map(entity => entity.getDroppedItem?.())
    .filter(item => item?.type === dirt).reduce((sum,item) => sum+item.count,0);
  const message = '刚才投出了六十四块，你走远以后我停了，另外两块还没丢。';
  player.chat('那就给我六十六块泥土。');
  await until(() => ground() === 64,'独立玩家看到第一堆六十四块已在场');
  fixture.send('tp DfyGuestCheck 6.5 -60 0.5');
  await until(() => player.entity.position.x === 6.5 && whispers.includes(message),'玩家移开后收到原委托的实际停投结果');
  const actions = readRows(fixture,'game-actions.jsonl').filter(row => row.skill === 'give_item');
  assert.equal(actions.length,1);
  const [job] = actions, check = job.result.recipientCheck;
  assert.deepEqual([job.exited,job.result.reasonCode,job.result.transfer.submitted,job.result.transfer.returnedClicks,
    job.result.receipt.thrown,job.result.receipt.recipientPicked], [true,'handoff_recipient_distant',64,1,64,0]);
  assert.deepEqual([check.observation,check.recipient,check.target.distance,check.target.position.x,check.remaining],
    [job.arguments.observation,player.player.uuid,6,6.5,2]);
  assert.deepEqual([ground(),fixture.materialCount('dirt'),player.inventory.count(dirt),whispers],[64,6,0,[initialReply,message]]);
}

// 用独立玩家核对备料询问、投物在途发言与最终实际收件。
// 步骤：读回初始库存 → 发送委托并按场景控制收件时机 → 核对实际发言、原工作和库存。
async function checkHandoff(fixture, player, whispers, mode, other) {
  const interrupt = mode === '--handoff-progress-probe';
  const feedback = mode === '--handoff-feedback-probe';
  const staleFeedback = mode === '--handoff-stale-feedback-probe';
  const late = interrupt || feedback || mode === '--handoff-late-probe', witnessed = mode !== '--handoff-unobserved-probe';
  const actor = player.players.DfyCompanion.entity;
  await until(() => fixture.materialCount('dirt') === 70 && player.inventory.items().length === 0 &&
    actor.position.distanceTo(player.entity.position) <= 3, '角色持有两堆泥土，原玩家空背包同场');
  const reply = '我身上有七十块普通泥土，从两个槽位能拿出六十六块。';
  await measurePlayerFeedback(player, '你身上能分给我六十六块泥土吗？', reply, () => until(() => whispers.includes(reply), '实际库存询问回复'),
    sample => writeFileSync(join(fixture.evidence, 'player-feedback.json'), JSON.stringify(sample, null, 2)));
  const actions = readRows(fixture, 'game-actions.jsonl');
  const query = actions.find(row => row.result.handoff);
  const sample = query.result.handoff;
  assert.deepEqual([sample.id, sample.recipient, sample.actor.uuid, sample.target.entityId], [query.work, player.player.uuid, actor.uuid, player.entity.id]);
  assert.deepEqual(sample.stock.selected.map(slot => [slot.held, slot.take]), [[64,64],[6,2]]);
  assert.deepEqual([fixture.materialCount('dirt'), player.inventory.count(player.registry.itemsByName.dirt.id)], [70,0]);
  assert.deepEqual(whispers, [reply]);
  const partial = mode === '--handoff-partial-probe';
  if (late || other || staleFeedback || partial) {
    fixture.send('clear DfyGuestCheck');
    fixture.send('give DfyGuestCheck minecraft:stone 2304');
    await until(() => player.inventory.count(player.registry.itemsByName.stone.id) === 2304,'收件玩家的 36 个槽位已满');
  }
  if (partial) return checkPartialHandoff(fixture,player,whispers,reply);
  if (other) {
    fixture.send('clear DfyCollector');
    fixture.send('tp DfyCollector 3.5 -60 0.5');
    await until(() => other.entity.position.x === 3.5 && other.inventory.count(other.registry.itemsByName.dirt.id) === 0,'第三位协议玩家等在物品落点');
  }
  const sent = other ? '这六十六块泥土被旁边的另一位玩家捡到了。' : late ? '六十六块泥土都已经丢出来了，暂时还没看到你捡到。' :
    witnessed ? '你已经捡到六十六块泥土，我的背包还剩四块。' : '我这边背包少了六十六块泥土，还不能确认你捡到了多少。';
  const during = '已经投出六十六块，这会儿还在等你捡。';
  const pending = () => readRows(fixture,'trace/events.jsonl').filter(row => row.kind === 'mail.received' &&
    row.category === 'mc.partner.motion' && row.body.status === 'awaiting_receipt' && row.body.progress.thrown === 66 &&
    row.body.progress.pickupUnknown === 66 && row.body.progress.coverage === 'read');
  await measurePlayerFeedback(player,'那就给我六十六块泥土。',sent,async () => {
    if (staleFeedback) {
      const waiting = await until(() => existsSync(join(fixture.evidence,'host/model-events.jsonl')) &&
        readRows(fixture,'model-events.jsonl').find(row => row.waitingForPickup),'进展答复已在固定后端等待');
      fixture.send('clear DfyGuestCheck minecraft:stone');
      await until(() => player.inventory.count(player.registry.itemsByName.dirt.id) === 66 &&
        readRows(fixture,'trace/events.jsonl').some(row => row.category === 'mc.partner.action' &&
          row.body.bridgeWork === waiting.waitingForPickup && row.body.result.receipt?.recipientPicked === 66),
        '原玩家已收件且宿主已归档原投物工作');
      writeFileSync(join(fixture.evidence,'host/pickup-confirmed'),waiting.waitingForPickup);
    }
    if (interrupt) {
      const current = await until(() => pending()[0],'原工作已投出，尚待收取');
      await measurePlayerFeedback(player,'东西已经丢出来了吗？',during,() => until(() => whispers.includes(during),'交物等待中的实际答复'),
        sample => writeFileSync(join(fixture.evidence,'progress-feedback.json'),JSON.stringify({ ...sample, work:current.body.action.bridgeWork },null,2)));
    }
    await until(() => whispers.includes(sent),'按原观察投物后说明可核实的效果');
    if (feedback) {
      assert.equal(readRows(fixture,'game-actions.jsonl').some(row => row.skill === 'give_item'),false);
      assert.equal(player.inventory.count(player.registry.itemsByName.dirt.id),0);
    }
  },
    sample => writeFileSync(join(fixture.evidence,'handoff-feedback.json'),JSON.stringify(sample,null,2)));
  if (feedback) await until(() => readRows(fixture,'game-actions.jsonl').some(row => row.skill === 'give_item'),
    '已向玩家报告进展的投物工作随后实际退出');
  const jobs = readRows(fixture, 'game-actions.jsonl').filter(row => row.skill === 'give_item');
  assert.equal(jobs.length,1);
  const [job] = jobs;
  assert.deepEqual([job.arguments.recipient,job.result.transfer.submitted,job.result.transfer.stockAfter,fixture.materialCount('dirt')],
    [player.player.uuid,66,4,4]);
  const proof = job.result.receipt;
  assert.deepEqual([job.result.status,proof.thrown,proof.recipientPicked,proof.otherPicked,proof.throwUnknown],
    other ? ['blocked',66,0,66,0] : late ? ['blocked',66,0,0,0] : witnessed ? ['done',66,66,0,0] : ['blocked',0,0,0,66]);
  if (witnessed) assert.deepEqual(proof.stacks.map(stack => stack.count),[64,1,1]);
  const expected = interrupt ? [reply,during,sent] : [reply,sent];
  assert.deepEqual(whispers,expected);
  if (feedback || staleFeedback) {
    const requests = await until(() => {
      const inputs = readRows(fixture,'trace/events.jsonl').filter(row => row.kind === 'model.prepared' && row.step === 1)
        .map(row => readPartnerContext(row).input);
      return inputs.some(input => input.trigger === 'plan.finished' && input.taskPlan.nodes.some(node => node.work === job.work)) && inputs;
    },'同一投物计划的完整结算已送模');
    const notices = requests.filter(input => input.trigger === 'handoff.progress');
    assert.equal(notices.length,1);
    const [notice] = notices;
    assert.deepEqual([notice.replyOrigin.bridgeWork,notice.taskPlan.origin.sequence,notice.taskPlan.origin.text],
      [job.work,job.order,'那就给我六十六块泥土。']);
    assert.ok(notice.recentDialogue.some(line => line.speaker === 'player' && line.order === job.order));
    assert.equal(notice.task.lastAction,undefined);
    assert.equal(notice.lastObservation,undefined);
    assert.equal(notice.taskPlan.nodes,undefined);
    assert.equal(notice.taskPlan.origin.sceneAtMessage,undefined);
    assert.ok(notice.recentEvents.every(event => !event.sceneAtMessage));
  }
  if (staleFeedback) assert.deepEqual(readRows(fixture,'model-events.jsonl').filter(row => row.returnedOldProgress),
    [{ returnedOldProgress:job.work }]);
  if (interrupt) assert.deepEqual(pending().map(row => row.body.action.bridgeWork),[job.work]);
  if (feedback) {
    const events = await until(() => {
      const actions = readRows(fixture,'trace/events.jsonl').filter(row => row.category === 'mc.partner.action');
      return actions.some(row => row.body.bridgeWork === job.work) && actions;
    },'按原投物工作核对发言和实际退出在宿主中的顺序');
    const replies = events.filter(row => row.body.result?.text === sent);
    assert.equal(replies.length,1);
    assert.deepEqual([replies[0].body.replyOrigin.sourceMessage,replies[0].body.replyOrigin.bridgeWork],[job.order,job.work]);
    assert.ok(replies[0].seq < events.find(row => row.body.bridgeWork === job.work).seq);
  }
  if (other) {
    assert.deepEqual(proof.collectors,[{ uuid: other.player.uuid, count: 66 }]);
    assert.equal(other.inventory.count(other.registry.itemsByName.dirt.id),66);
    assert.equal(player.inventory.count(player.registry.itemsByName.dirt.id),0);
    fixture.send('clear DfyGuestCheck minecraft:stone');
    return;
  }
  if (late) {
    assert.equal(player.inventory.count(player.registry.itemsByName.dirt.id),0);
    const original = await fixture.repeatHandoff(job,job.work);
    assert.deepEqual(original.action.result,job.result);
    const replacement = await fixture.repeatHandoff(job,'retry.'+job.work,interrupt ? job.order+1 : job.order);
    assert.deepEqual([replacement.action.exited,replacement.action.result.reasonCode,replacement.action.result.originalWork],
      [true,'handoff_already_started',job.work]);
    assert.equal(replacement.action.result.transfer,undefined);
    fixture.send('clear DfyGuestCheck minecraft:stone');
  }
  await until(() => player.inventory.count(player.registry.itemsByName.dirt.id) === 66, '独立玩家在生存模式实际拾到六十六块', 12000);
  if (late) {
    player.chat('看看我后来捡到刚才那批了没。');
    const final = '刚复查了那次交物，你后来把六十六块都捡到了。';
    await until(() => whispers.includes(final), '迟到拾取沿第一次交物核对');
    const all = readRows(fixture, 'game-actions.jsonl');
    const check = all.find(row => row.result.handoffReceipt)?.result.handoffReceipt;
    assert.deepEqual([check.work,check.order,check.sourceThrough,check.originalStatus,check.receipt.recipientPicked],
      [job.work,job.order,proof.throughSeq,'blocked',66]);
    assert.equal(all.filter(row => row.skill === 'give_item' && row.result.transfer?.submitted > 0).length,1);
    assert.deepEqual(whispers,[...expected,final]);
  }
}

// 由独立玩家核对追问回到原消息并实际补齐相隔的两处开口。
// 步骤：观察原始缺格 → 收到追问后用自然语言回答 → 核对实际方块、库存和投递。
async function checkOpeningGroup(fixture, player, whispers) {
  const names = () => [3, 7].flatMap(x => [-60, -59].map(y => player.blockAt(player.entity.position.clone().set(x, y, -3))?.name));
  await until(() => names().every(name => name === 'air') && fixture.materialCount('dirt') === 8,
    '两处竖向开口及八块泥土已就位');
  const feedbackPath = join(fixture.evidence, 'player-feedback.json');
  const message = '帮我把墙上的缺口补好';
  const question = '两处缺口都要补吗？';
  let sample;
  await measurePlayerFeedback(player, message, question,
    () => until(() => whispers.includes(question), '收到本地追问'), result => {
      sample = result;
      writeFileSync(feedbackPath, JSON.stringify(result, null, 2));
    });
  assert.deepEqual(names(), ['air', 'air', 'air', 'air']);
  player.chat('都补');
  await until(() => whispers.includes('两处缺口都补齐了。'), '收到实际补齐结果', 40000);
  assert.deepEqual(names(), ['dirt', 'dirt', 'dirt', 'dirt']);
  assert.equal(fixture.materialCount('dirt'), 4);
  assert.deepEqual(whispers, ['两处缺口都要补吗？', '两处缺口都补齐了。']);
  const rows = readRows(fixture, 'trace/events.jsonl');
  const answer = rows.find(row => row.category === 'mc.partner.question' && row.body.status === 'resolved').body;
  assert.deepEqual([answer.sourceMessage, answer.answerMessage, answer.answer], [2, 3, '都补']);
  const input = rows.find(row => row.kind === 'model.prepared' && row.step === 1 &&
    readPartnerContext(row).input.events.some(event => event.sequence === answer.sourceMessage && event.text === message));
  const presented = rows.find(row => row.category === 'mc.partner.question' && row.body.status === 'presented' &&
    row.body.order === answer.sourceMessage && row.body.item === answer.item);
  const actions = readRows(fixture, 'game-actions.jsonl');
  assert.ok(input && presented, '原话送模回合与同一事项的追问呈现');
  const raw = actions.find(action => action.result.openingObservation);
  const detail = actions.find(action => action.result.sourceObservation);
  assert.deepEqual(detail.result.sourceObservation, raw.result.openingObservation);
  assert.equal(detail.result.sampledAtMs, raw.result.sampledAtMs);
  assert.deepEqual(detail.result.survey, raw.result.survey);
  const summary = JSON.parse(rows.find(row => row.kind === 'tool.exited' && row.callId === 'look-at-openings').output);
  assert.equal(summary.openingObservation.id, raw.work);
  assert.ok(Buffer.byteLength(JSON.stringify(summary)) < Buffer.byteLength(JSON.stringify(raw.result)));
  const delivery = actions.find(action => action.work === presented.body.bridgeWork);
  assert.ok(delivery?.exited && delivery.result.sent && delivery.result.text === question);
  assert.deepEqual([delivery.order, delivery.generation], [answer.sourceMessage, presented.body.generation]);
  assert.ok(Number.isFinite(sample.elapsedMs));
  writeFileSync(feedbackPath, JSON.stringify({ ...sample, sourceMessage: answer.sourceMessage,
    session: input.session, run: input.run, work: delivery.work,
    modelThinking: { enabled: input.thinking, effort: input.reasoningEffort } }, null, 2));
  const placed = actions.find(row => row.skill === 'fill_target');
  assert.deepEqual([placed.exited, placed.result.sourceMessage, placed.result.changes.length], [true, 2, 4]);
}

// 用同一面墙和初始库存核对不同玩家授权的首反馈和实际放置格。
// 步骤：从玩家发话计时 → 按实际材料缺口补给 → 对照玩家视野、原观察与执行回执。
async function checkOpeningScope(fixture, player, whispers, left) {
  const columns = [3, 7];
  const blocks = () => columns.flatMap(x => [-60, -59].map(y => player.blockAt(player.entity.position.clone().set(x,y,-3))?.name));
  await until(() => blocks().every(name => name === 'air') && fixture.materialCount('dirt') === 2, '同场两处开口及两块泥土');
  const message = left ? '只补左边的缺口' : '墙上两个缺口都补';
  const feedback = left ? '左边的缺口有两格，我有两块泥土，够补。' : '两处缺口共四格，我有两块泥土，还差两块。';
  const path = join(fixture.evidence, 'player-feedback.json');
  let sample;
  await measurePlayerFeedback(player, message, feedback,
    () => until(() => whispers.includes(feedback), '收到与原目标和材料相符的反馈'), result => {
      sample = result;
      writeFileSync(path, JSON.stringify(result, null, 2));
    });
  if (!left) {
    assert.deepEqual(blocks(), ['air','air','air','air']);
    fixture.send('give DfyCompanion minecraft:dirt 2');
    await until(() => fixture.materialCount('dirt') === 4, '两块补给进入角色背包');
    player.chat('缺的两块泥土给你了，按原范围补');
  }

  const completion = left ? '左边的缺口补好了。' : '两处缺口都补齐了。';
  await until(() => whispers.includes(completion), '玩家收到实际放置结果', 40000);
  assert.deepEqual(whispers, [feedback, completion]);
  assert.deepEqual(blocks(), left ? ['dirt','dirt','air','air'] : ['dirt','dirt','dirt','dirt']);
  assert.equal(fixture.materialCount('dirt'), 0);
  const actions = readRows(fixture, 'game-actions.jsonl');
  const original = actions.find(row => row.result.openingObservation);
  const speech = actions.find(row => row.skill === 'reply' && row.result.text === feedback);
  const fills = actions.filter(row => row.skill === 'fill_target');
  assert.deepEqual([original.result.openingObservation.message, original.result.inventory.dirt, speech.order, speech.result.sent], [2, 2, 2, true]);
  const rows = readRows(fixture, 'trace/events.jsonl');
  const summary = JSON.parse(rows.find(row => row.kind === 'tool.exited' && row.callId === 'look-at-openings').output);
  assert.equal(summary.openingObservation.id, original.work);
  const wall = summary.playerFocus.candidates.filter(gap => gap.block === 'dirt' && gap.axis === 'x' && gap.z === -3 && gap.y === -60)
    .sort((a, b) => a.x - b.x);
  assert.deepEqual(wall.map(gap => [gap.x, gap.width, gap.height]), [[3,1,2],[7,1,2]]);
  const selected = left ? wall.slice(0, 1) : wall;
  const expectedCells = selected.flatMap(gap => Array.from({ length: gap.height }, (_, dy) => [gap.x,gap.y+dy,gap.z].join(','))).sort();
  const materials = { needed: expectedCells.length, available: summary.inventory.dirt,
    missing: Math.max(0, expectedCells.length - summary.inventory.dirt) };
  assert.equal(materials.missing, left ? 0 : 2);
  assert.deepEqual(fills.map(row => [row.result.sourceMessage,
    row.result.changes.map(({ x, y, z }) => [x,y,z].join(',')).sort()]), [[2, expectedCells]]);
  const turn = rows.find(row => row.kind === 'model.prepared' && row.step === 1 &&
    readPartnerContext(row).input.events.some(event => event.sequence === 2 && event.text === message));
  const delivery = rows.find(row => row.category === 'mc.partner.action' && row.body.bridgeWork === speech.work);
  assert.equal(delivery.body.session, turn.session);
  writeFileSync(path, JSON.stringify({ ...sample, sourceMessage: 2, session: turn.session, run: turn.run,
    work: speech.work, observation: original.work, candidates: selected.map(gap => gap.id), materials,
    modelThinking: { enabled: turn.thinking, effort: turn.reasoningEffort } }, null, 2));
}

// 在真实世界中验证高台掉队、取材后垫高和原跟随恢复。
// 步骤：核对初始材料及高度 → 触发受阻和新指令 → 核对取材、落块及站位 → 核实跟随恢复及退出。
async function checkLayeredFollow(fixture, player, whispers, resupply) {
  const trace = () => readRows(fixture, 'trace/events.jsonl');
  const mail = type => trace().filter(row => row.kind === 'mail.received' && row.category === 'mc.partner.' + type);
  const { pads, sources, foothold, upper } = layeredStands;
  const block = point => player.blockAt(player.entity.position.clone().set(...point))?.name;
  await until(() => fixture.materialCount('dirt') === (resupply ? 0 : 2) && pads.every(point => block(point) === 'air') &&
    (!resupply || sources.every(point => block(point) === 'dirt')) &&
    block(foothold) === (resupply ? 'stone' : 'air') &&
    block([34, -57, -14]) === 'stone', '游戏读回两格空位与高台');
  player.chat('大肥鱼，跟着我');
  const original = await until(() => mail('motion').find(row => row.body.status === 'accepted'), '原跟随接纳');
  await until(() => whispers.includes('跟着呢。'), '玩家收到初始接话');
  fixture.send(`tp DfyGuestCheck ${upper.join(' ')} facing 32.5 -59 -12.5`);
  const raised = await until(() => mail('attention').find(row => row.body.status === 'raised'), '高台产生一次跟随思考', 15000);
  await until(() => whispers.includes('上面有点高，我先找找路。'), '玩家收到受阻说明');
  player.chat('能听见吗');
  await until(() => whispers.includes('听得见。'), '受阻时仍能聊天');
  await until(() => mail('motion').some(row => row.body.status === 'blocked'), '原物理跟随进入受阻');
  player.chat(resupply ? '你挖俩方块垫一下' : '你可以垫上来');
  if (resupply) {
    await until(() => mail('plan').some(row => row.body.status === 'queued' && row.body.plan.remainingGoal), '原委托已提交取材');
    player.chat('我在上面等你');
    await until(() => whispers.includes('收到，我在取材。'), '原计划执行期间仍答复新消息');
  }
  await until(() => whispers.includes('我踩到第二格了。'), '两次放置和精确移动均已结算', 40000);
  assert.deepEqual(pads.map(block), ['dirt', 'dirt']);
  assert.equal(fixture.materialCount('dirt'), 0);
  const layers = mail('plan').filter(row => row.body.status === 'settled');
  const actions = readRows(fixture, 'game-actions.jsonl');
  if (resupply) {
    const queued = mail('plan').filter(row => row.body.status === 'queued').map(row => row.body.plan);
    assert.equal(queued[1].supersedes, queued[0].id);
    assert.deepEqual(queued[1].origin, queued[0].origin);
    const gathered = layers.shift().body.nodes;
    assert.deepEqual(gathered.map(node => [node.skill, node.result.inventoryDelta?.dirt]), [['dig_block', 1], ['dig_block', 1]]);
    assert.deepEqual(sources.map(block), ['air', 'air']);
    for (const [index, node] of gathered.entries()) {
      const receipt = actions.find(action => action.work === node.work);
      assert.deepEqual(receipt?.result.changes.map(cell => [cell.x, cell.y, cell.z, cell.before, cell.after]),
        [[...sources[index], 'dirt', 'air']]);
    }
  }
  assert.deepEqual(layers.map(row => row.body.nodes[0].skill), ['place', 'move', 'place', 'move']);
  const queue = trace().find(row => row.kind === 'tool.exited' && row.callId === 'steps');
  const next = trace().find(row => row.kind === 'model.prepared' && row.seq > queue.seq);
  const settlement = JSON.parse(scenarioInput(next).messages.at(-1).content);
  assert.equal(settlement.trigger, 'plan.finished');
  assert.deepEqual(settlement.planSettlements, layers.map(row => row.body));
  for (const [i, row] of layers.entries()) {
    assert.equal(row.body.layer, i + 1);
    const node = row.body.nodes[0];
    const work = actions.find(action => action.work === node.work);
    assert.ok(work?.exited && work.result.status === 'done');
    if (i % 2 === 0) assert.equal(work.result.placed, true);
    else assert.deepEqual(['x', 'y', 'z'].map(axis => Math.floor(work.result.position[axis])),
      [pads[(i - 1) / 2][0], pads[(i - 1) / 2][1] + 1, -13]);
  }
  const work = original.body.action.work;
  if (!resupply) {
    await until(() => mail('motion').find(row => row.seq > queue.seq && row.body.action.work === work &&
      row.body.status === 'blocked' && row.body.progress?.vertical >= 2 && row.body.progress?.path === 'unreachable'),
    '到达第二格后仍没有可达路径');
    assert.deepEqual(mail('attention').map(row => row.body.status), ['raised']);
    fixture.send(`tp DfyGuestCheck ${pads[0][0] + 0.5} ${pads[0][1] + 1} ${pads[0][2] + 0.5}`);
  }
  const recovered = await until(() => mail('attention').find(row => row.body.status === 'recovered'), '原跟随已稳定恢复');
  const accepted = mail('motion').filter(row => row.body.status === 'accepted' && row.body.action.skill === 'follow');
  assert.ok(accepted.length >= 2);
  for (const { body: { action } } of accepted)
    assert.deepEqual([action.work, action.bridgeWork], [work, action.segment === 1 ? work : `${work}.resume.${action.segment}`]);
  assert.deepEqual([raised.body.work, recovered.body.work], [work, work]);
  assert.deepEqual(mail('attention').map(row => row.body.status), ['raised', 'recovered']);
  player.chat('不用跟了');
  const lastSegment = accepted.at(-1).body.action.bridgeWork;
  await until(() => mail('action').some(row => row.body.skill === 'follow' && row.body.work === work &&
    row.body.bridgeWork === lastSegment && row.body.result.status === 'interrupted'), '原跟随的真实退出回执');
  assert.deepEqual(whispers, ['跟着呢。', '上面有点高，我先找找路。', '听得见。',
    ...(resupply ? ['收到，我在取材。'] : []), '我垫两步上来。', '我踩到第二格了。']);
}

// 验证跟随通过同一 work 跨水和聊天，再验证固定目标反向抵达干岸。
// 步骤：确认低水面夹具 → 跨水时发话 → 干岸叫停 → 反向移动及核对退出。
async function checkWater(fixture, player, whispers) {
  const rows = () => readRows(fixture, 'trace/events.jsonl').filter(row => row.kind === 'mail.received');
  await until(() => player.blockAt(player.entity.position.clone().set(-31, -61, -24))?.name === 'water',
    '读回水道夹具');
  player.chat('跟我渡水');
  const accepted = await until(() => rows().find(row => row.category === 'mc.partner.motion' && row.body.status === 'accepted'),
    '启动渡水跟随');
  fixture.send(`tp DfyGuestCheck ${waterCourse.east.join(' ')}`);
  await until(() => rows().some(row => row.body.progress?.terrain === 'swimming'), '跟随者真正入水', 18000);
  player.chat('水里还跟着吗');
  await until(() => whispers.includes('我还跟着你。'), '渡水时可发言');
  await until(() => rows().some(row => row.body.progress?.terrain === 'dry' && row.body.status === 'in_range'),
    '同一跟随渡水并站稳干岸', 18000);
  player.chat('这里停下');
  const followed = await until(() => rows().find(row => row.category === 'mc.partner.action' && row.body.skill === 'follow'),
    '叫停后跟随实际退出');
  assert.equal(followed.body.bridgeWork, accepted.body.action.bridgeWork);
  assert.equal(followed.body.result.status, 'interrupted');
  assert.equal(followed.body.result.navigation.crossings, 1);
  assert.ok(followed.body.result.position.x > -28);
  assert.ok(rows().filter(row => row.body.action?.skill === 'follow')
    .every(row => row.body.action.bridgeWork === accepted.body.action.bridgeWork));
  player.chat('回到西岸');
  const moved = await until(() => rows().find(row => row.category === 'mc.partner.action' && row.body.skill === 'move'),
    '固定目的地反向渡水', 30000);
  assert.equal(moved.body.result.status, 'done');
  const moving = rows().filter(row => row.category === 'mc.partner.motion' && row.body.action.bridgeWork === moved.body.bridgeWork &&
    row.body.progress?.phase === 'moving');
  assert.ok(moving.length && moving[0].body.progress.distance > moved.body.result.progress.distance);
  assert.equal(moving[0].body.progress.startDistance, moved.body.result.progress.startDistance);
  assert.ok(moved.body.result.progress.startDistance > moved.body.result.progress.distance);
  assert.equal(moved.body.result.progress.phase, 'arrived');
  assert.deepEqual(moved.body.result.progress.position, moved.body.result.position);
  assert.deepEqual(moved.body.result.navigation, { crossings: 1, phase: 'dry' });
  assert.ok(moved.body.result.position.x < -34);
  player.chat('去封住的地方');
  const blocked = await until(() => rows().find(row => row.seq > moved.seq && row.category === 'mc.partner.action' &&
    row.body.skill === 'move'), '封闭目标给出失败回执', 40000);
  assert.equal(blocked.body.result.status, 'blocked');
  assert.equal(blocked.body.result.reasonCode, 'move_no_path');
  assert.equal(blocked.body.result.progress.phase, 'no_path');
  assert.ok(blocked.body.result.reason);
  assert.ok(!readRows(fixture, 'game-actions.jsonl').some(row => ['collect', 'place'].includes(row.skill)));
}

// 用独立世界读回检查墙格保护、未采满时的逐格证据和后续对话。
// 步骤：发出取材边界 → 等待动作与重采样 → 对照回执及实际块名。
async function checkMaterials(fixture, player, whispers) {
  player.chat('附近可以挖，从地上挖两块泥土，别拆墙。');
  await until(() => existsSync(join(fixture.evidence, 'host/game-actions.jsonl')) &&
    readRows(fixture, 'game-actions.jsonl').some(row => row.skill === 'observe' && row.order === 2),
    '部分采集后重新观察', 30000);
  const work = readRows(fixture, 'game-actions.jsonl').filter(row => row.skill === 'collect');
  assert.equal(work.length, 2);
  assert.ok(work.every(row => row.arguments.source === 'ground' && row.result.status === 'blocked'));
  assert.deepEqual(work.map(row => row.result.reasonCode), ['dig_outside_scope', 'dig_source_missing']);
  assert.deepEqual(work[0].result.changes, []);
  assert.deepEqual(work[1].result.changes, [{ x: 2, y: -61, z: 0, before: 'dirt', after: 'air' }]);
  assert.deepEqual(work[1].result.inventoryDelta, { dirt: 1 });
  const block = (x, y, z) => player.blockAt(player.entity.position.clone().set(x, y, z))?.name;
  assert.equal(block(2, -61, 0), 'air');
  assert.equal(block(2, -59, -3), 'dirt');
  player.chat('刚才挖了哪里？');
  await until(() => whispers.includes('我只挖了地上的一格泥土。'), '重读后知道已改动的方块');
}

// 核对真实跟随的暂停、单格入包和原工作恢复。
// 步骤：边走边聊 → 提交单格指令并等待原动作退出 → 核对恢复后显式停止。
async function checkFollowDig(fixture, player, whispers) {
  const mail = category => readRows(fixture, 'trace/events.jsonl').filter(row =>
    row.kind === 'mail.received' && row.category === 'mc.partner.' + category);
  const accepted = segment => until(() => mail('motion').find(row => row.body.status === 'accepted' &&
    row.body.action.skill === 'follow' && row.body.action.segment === segment), '跟随实际进入执行段 ' + segment);
  player.chat('你跟着我走');
  const first = (await accepted(1)).body.action;
  await walkCourse(player, [0.5, -60, -0.5], '你还跟着吗');
  await until(() => whispers.includes('嗯，跟着呢。'), '走动中的实际发言');
  player.chat('把这个草方块挖掉');
  await until(() => whispers.includes('你指哪一格？'), '自然追问已发出');
  player.chat('你听得见吗');
  await until(() => whispers.includes('听得见。'), '旧问题等待时仍可接话');
  // 指向方块顶面中心，给协议量化的朝向和移动结束时的站位留出余量。
  await player.lookAt(player.entity.position.clone().set(2.5, -60.02, 0.5), true);
  await until(() => Math.sin(fixture.ownerEntity()?.pitch) < -0.4, '陪玩端已收到指向草方块的俯角');
  player.chat('我面前的这个');
  const resumed = (await accepted(2)).body.action;
  const effects = mail('action');
  const paused = effects.find(row => row.body.work === first.work && row.body.transition === 'paused');
  const dig = effects.find(row => row.body.skill === 'collect' && row.body.result?.inventoryDelta?.dirt === 1);
  assert.ok(paused && dig && paused.seq < dig.seq);
  assert.equal(resumed.work, first.work);
  assert.notEqual(resumed.bridgeWork, first.bridgeWork);
  player.chat('先别跟了');
  const exit = await until(() => mail('action').find(row => row.body.work === first.work && row.body.transition === 'exited'),
    '明确叫停后的原工作退出');
  assert.equal(exit.body.bridgeWork, resumed.bridgeWork);
  assert.equal(exit.body.result.status, 'interrupted');
  await until(() => fixture.settled >= 7, '跟随停止原话已处理');
}

// 用实际插话和另一客户端拾取，核对原采掘在途反馈、受阻原因和双方库存。
// 步骤：发起取材 → 在实际开挖时插话并计时 → 对照真实掉落、拾取人和结算发言。
async function checkPickup(fixture, player, whispers) {
  const point = player.entity.position.clone().set(73, -60, 0);
  await until(() => player.entity.position.x === 72.5 && player.blockAt(point)?.name === 'grass_block', '取材格与独立拾取人就位');
  const events = () => readRows(fixture, 'trace/events.jsonl');
  const feedback = '我正在挖草方块，泥土还没进背包。';
  const completion = '这块泥土是你捡到的，我的背包里还是零块。';
  const collectPacket = packet => packet.collectorEntityId === player.entity.id && packet.pickupItemCount === 1;
  const packets = [];
  const onCollect = packet => packets.push(packet);
  // 独立玩家看到泥土生成才进入已经挖开的格，由服务器正常判定拾取。
  const onDrop = entity => {
    if (entity.position.distanceTo(point) < 2 && entity.getDroppedItem?.()?.name === 'dirt')
      fixture.send('tp DfyGuestCheck 73.5 -60 0.5');
  };
  player.on('itemDrop', onDrop);
  player._client.on('collect', onCollect);
  try {
    player.chat('帮我就地取一块泥土。');
    const digging = await until(() => events().find(row => row.category === 'mc.partner.motion' && row.body.status === 'digging'),
      '宿主按原工作读到实际开挖');
    const path = join(fixture.evidence, 'player-feedback.json');
    await measurePlayerFeedback(player, '挖得怎么样了？', feedback, () => until(() => whispers.includes(feedback), '采掘中的实际反馈'),
      sample => writeFileSync(path, JSON.stringify({ ...sample, bridgeWork: digging.body.action.bridgeWork }, null, 2)));
    await until(() => whispers.includes(completion), '角色报告由原玩家拾取', 20000);
    const actions = readRows(fixture, 'game-actions.jsonl');
    const jobs = actions.filter(row => row.skill === 'collect');
    assert.equal(jobs.length, 1);
    const [job] = jobs;
    assert.equal(job.work, digging.body.action.bridgeWork);
    assert.deepEqual([job.result.status, job.result.reasonCode, job.result.broken, job.result.stock.after],
      ['blocked', 'pickup_other_collector', 1, 0]);
    const entry = job.result.pickups.recent[0];
    assert.ok(packets.some(packet => collectPacket(packet) && packet.collectedEntityId === entry.entityId));
    assert.equal(player.inventory.items().filter(item => item.name === 'dirt').reduce((n, item) => n + item.count, 0), 1);
    assert.equal(fixture.materialCount('dirt'), 0);
    assert.equal(player.blockAt(point).name, 'air');
    assert.deepEqual(whispers, [feedback, completion]);
    const reply = actions.find(row => row.skill === 'reply' && row.result.text === feedback);
    const prepared = events().filter(row => row.kind === 'model.prepared' && row.step === 1 &&
      row.session === digging.body.action.session).map(readPartnerContext)
      .find(turn => turn.input.events.some(event => event.sequence === reply.order && event.text === '挖得怎么样了？'));
    assert.equal(prepared.input.activeMotion.work, job.work);
    assert.match(prepared.guidance.domainExperience.collectionProgress, /最近采样/);
    assert.equal(prepared.guidance.domainExperience.nearbyMaterials, undefined);
    const pickupUpdates = events().filter(row => row.category === 'mc.partner.motion' &&
      row.body.action?.bridgeWork === job.work && row.body.progress?.pickup).map(row => row.body.progress.pickup);
    assert.ok(pickupUpdates.some(p => p.entityId === entry.entityId && p.item === 'dirt'));
    assert.ok(pickupUpdates.every((p, i) => !i || JSON.stringify(p) !== JSON.stringify(pickupUpdates[i-1])));
    const sample = JSON.parse(readFileSync(path, 'utf8'));
    assert.ok(Number.isFinite(sample.elapsedMs));
    writeFileSync(path, JSON.stringify({ ...sample, sourceMessage: reply.order, replyWork: reply.work,
      progress: prepared.input.activeMotion.progress }, null, 2));
    fixture.send('tp DfyGuestCheck 72.5 -60 0.5');
    fixture.send('setblock 73 -60 0 minecraft:grass_block');
    fixture.send('clear DfyCompanion');
    fixture.send('give DfyCompanion minecraft:stone 2304');
    await until(() => fixture.materialCount('stone') === 36 * 64 && player.blockAt(point)?.name === 'grass_block',
      '背包与快捷栏装满，独立玩家读到复位后的草方块');
    const full = '背包和快捷栏都满了，草方块还在原处。';
    player.chat('再帮我取一块泥土。');
    await until(() => whispers.includes(full), '满包取材按实际容量结算');
    const attempts = readRows(fixture, 'game-actions.jsonl').filter(row => row.skill === 'collect');
    assert.equal(attempts.length, 2);
    assert.deepEqual([attempts[1].result.reasonCode, attempts[1].result.broken, attempts[1].result.changes,
      attempts[1].result.capacity.knownSpace], ['pickup_inventory_full', 0, [], 0]);
    assert.equal(player.blockAt(point).name, 'grass_block');
    assert.deepEqual(whispers, [feedback, completion, full]);
  } finally {
    player._client.removeListener('collect', onCollect);
    player.removeListener('itemDrop', onDrop);
    fixture.send('effect clear DfyCompanion minecraft:mining_fatigue');
    fixture.send('tp DfyGuestCheck 0.5 -60 -2.5');
  }
}

// 用独立游戏客户端核对场景读数、合成实效和玩家收到的结果。
// 步骤：确认布置 → 发出固定原话 → 比对游戏回执、模型读回和实际发言。
async function checkCraft(fixture, player, whispers) {
  await until(() => fixture.materialCount('oak_log') === 1, '测试原木实际入包');
  const cell = player.entity.position.clone().set(3, -60, 0);
  await until(() => player.blockAt(cell)?.name === 'dirt', '独立客户端读到观察目标');
  const stack = (name, count) => Object.values(player.entities).find(entity => {
    const item = entity.name === 'item' && entity.getDroppedItem?.();
    return item?.name === name && item.count === count;
  });
  const nearby = await until(() => stack('dirt', 2), '独立客户端读到掉落的泥土');
  await until(() => stack('stone', 1) && stack('dirt', 4), '独立客户端读到异物和半径外泥土');
  assert.equal(fixture.materialCount('oak_planks'), 0);
  player.chat('看看你旁边，把原木做成四块木板。');
  await until(() => whispers.includes('原木已经做成四块橡木木板。'), '合成结果实际发言');

  assert.deepEqual(whispers, ['原木已经做成四块橡木木板。']);
  assert.equal(fixture.materialCount('oak_log'), 0);
  assert.equal(fixture.materialCount('oak_planks'), 4);
  const actions = readRows(fixture, 'game-actions.jsonl');
  const observation = actions.find(row => row.skill === 'observe');
  const sample = observation.result.observation.samples[0];
  assert.deepEqual(sample.views.map(view => [view.source, view.status]),
    [['self', 'sampled'], ['player', 'sampled'], ['message', 'sampled']]);
  assert.equal(sample.views[2].message, 2);
  assert.ok(Number.isInteger(sample.sampledAtMs));
  assert.equal(sample.verified[0].block, player.blockAt(cell).name);
  const dropped = actions.find(row => row.result.dropObservation)?.result.dropObservation;
  assert.equal(dropped.coverage, 'client_tracked_entities');
  assert.deepEqual(dropped.entities.map(e => [e.entityId, e.uuid, e.item, e.count]), [[nearby.id, nearby.uuid, 'dirt', 2]]);
  const target = actions.find(row => row.result.dropTarget).result.dropTarget;
  assert.deepEqual([target.observation, target.entity, target.entityId, target.count], [dropped.id, nearby.uuid, nearby.id, 2]);
  assert.equal(stack('dirt', 2)?.uuid, nearby.uuid);

  const crafts = actions.filter(row => row.skill === 'craft');
  assert.equal(crafts.length, 1);
  const [craft] = crafts;
  assert.deepEqual(craft.result.inventoryDelta, { oak_log: -1, oak_planks: 4 });
  const readback = readRows(fixture, 'trace/events.jsonl').find(row => row.kind === 'mail.received' &&
    row.category === 'mc.partner.action' && row.body.bridgeWork === craft.work);
  assert.deepEqual(readback.body.result.craft, craft.result.craft);
}

// 以独立客户端验证取材、四格放置和旁边门洞保持原样。
// 步骤：核对原境 → 发出补墙指令 → 按模式加入并解除遮挡 → 对照游戏和 Trace。
async function checkFillFour(fixture, player, whispers, combined) {
  const read = pos => player.blockAt(player.entity.position.clone().set(...pos))?.name;
  const material = [2, -61, 0];
  if (combined) {
    await checkFollowDig(fixture, player, whispers);
    assert.equal(fixture.materialCount('dirt'), 1);
    fixture.send('give DfyCompanion minecraft:oak_log 1');
    fixture.send('tp DfyGuestCheck 6.5 -60 -1.5 facing 3.5 -59 -3');
  }
  const source = combined ? 8 : 2;
  const collectionOrder = combined ? source + 1 : source;
  const independentPickups = [];
  const onPickup = packet => independentPickups.push(packet);
  player._client.on('collect', onPickup);
  await until(() => fixture.ownerEntity()?.position.x > 6 && read(material) === (combined ? 'air' : 'grass_block') &&
    fourCells.every(cell => read(cell) === 'air'), '四格、取材格原状和独立观察站位');
  const obstruction = fourCells[2];
  const traceRows = () => readRows(fixture, 'trace/events.jsonl');
  if (combined) {
    await until(() => fixture.materialCount('oak_log') === 1, '原木已入包');
    player.chat('把原木做成五个木板，然后填上我面前这堵墙的四个缺口');
    await until(() => whispers.includes('一根原木做不够五块木板，那四格我记着呢。'), '合成缺料后记住原开口');
    assert.ok(fourCells.every(cell => read(cell) === 'air'));
    fixture.send(`tp DfyGuestCheck ${wall.doorStand.join(' ')}`);
    await until(() => fixture.ownerEntity()?.position.x > 7, '玩家已经走到另一处门洞');
    player.chat('那你在地上挖四个泥土，把墙补了');
  } else {
    player.chat('从地上取够泥土，填上 x=3 到 4、y=-60 到 -59、z=-3 的四格，另一处缺口留着。');
  }
  await until(() => traceRows().some(row => row.kind === 'mail.received' && row.category === 'mc.partner.motion' &&
    row.body.action.order === collectionOrder && row.body.action.skill === 'collect' && row.body.status === 'accepted'), '地面取材已接单');
  if (combined) {
    fixture.send('setblock '+obstruction.join(' ')+' minecraft:stone');
    await until(() => whispers.includes('下面两格补好了，上面有块石头挡着。'), '按真实部分回执发言');
    assert.deepEqual(fourCells.map(read), ['dirt','dirt','stone','air']);
    assert.equal(fixture.materialCount('dirt'), 2);
    assert.equal(readRows(fixture, 'game-actions.jsonl').find(row => row.skill === 'fill_target').result.reasonCode, 'occupied');
    fixture.send('setblock '+obstruction.join(' ')+' minecraft:air');
    await until(() => read(obstruction) === 'air', '独立客户端读到遮挡已移除');
    player.chat('石头挪开了，把刚才剩下的补上。');
  } else {
    fixture.send('setblock 3 -61 -3 minecraft:air');
    fixture.send('setblock 2 -60 -3 minecraft:air');
    await until(() => read([3,-61,-3]) === 'air' && read([2,-60,-3]) === 'air', '首格原有支撑移除');
  }
  await until(() => whispers.includes('四格都补上了，另一处缺口留着。'), '实际补完四格后发言', 45000);
  const actions = readRows(fixture, 'game-actions.jsonl');
  const mutations = actions.filter(row => ['collect', 'place', 'fill_target'].includes(row.skill));
  assert.deepEqual(mutations.map(row => row.skill), combined ? ['collect','collect','fill_target','fill_target'] : ['collect','fill_target']);
  const collect = mutations.find(row => row.arguments.source === 'ground');
  player._client.removeListener('collect', onPickup);
  const fill = mutations.at(-1);
  assert.equal(collect.arguments.source, 'ground');
  assert.deepEqual(collect.arguments.protectedCells, fourCells.map(([x,y,z]) => ({ x,y,z })));
  assert.deepEqual(collect.result.stock, { item: 'dirt', before: combined ? 1 : 3, required: 4, after: 4 });
  assert.deepEqual(collect.result.inventoryDelta, { dirt: combined ? 3 : 1 });
  const evidence = collect.result.pickups;
  assert.deepEqual(evidence.selfCollected, collect.result.inventoryDelta);
  assert.deepEqual(evidence.otherCollected, {});
  for (const entry of evidence.recent) {
    assert.equal(entry.item, 'dirt');
    assert.equal(entry.selfCount, 1);
    assert.equal(entry.lastCollector.uuid, player.players.DfyCompanion.uuid);
    assert.ok(independentPickups.some(packet => packet.collectedEntityId === entry.entityId &&
      packet.collectorEntityId === entry.lastCollector.entityId && packet.pickupItemCount === entry.selfCount));
  }
  assert.equal(collect.result.changes.length, combined ? 3 : 1);
  const progress = traceRows().filter(row => row.category === 'mc.partner.motion' &&
    row.body.action?.bridgeWork === collect.work && row.body.progress).map(row => row.body.progress);
  const collecting = progress.find(step => step.phase === 'awaiting_pickup');
  assert.ok(collecting?.broken > 0 && collecting.lastBreakAtMs <= collecting.sampledAtMs);
  assert.ok(collect.result.changes.some(cell => ['x','y','z'].every(axis => cell[axis] === collecting.targetCell[axis])));
  assert.deepEqual([collect.result.progress.phase, collect.result.progress.broken, collect.result.progress.stock.current],
    ['finished', collect.result.changes.length, 4]);
  for (const ground of collect.result.changes) {
    assert.equal(ground.y, -61);
    assert.equal(ground.before, 'grass_block');
    assert.equal(ground.after, 'air');
    assert.equal(read([ground.x, ground.y, ground.z]), 'air');
  }
  assert.equal(fill.result.sourceMessage, source);
  assert.equal(fill.result.status, 'done');
  assert.deepEqual(fill.result.cells, fourCells.map(([x, y, z], index) => {
    const present = combined && index < 2;
    return { x, y, z, before: present ? 'dirt' : 'air', block: 'dirt', status: present ? 'present' : 'placed' };
  }));
  const applied = mutations.filter(row => row.skill === 'fill_target').flatMap(row => row.result.changes);
  if (!combined) assert.equal(applied[0].x, 4, '右下格先施工后给左下格提供支撑');
  applied.sort((a,b) => a.y - b.y || a.x - b.x);
  assert.deepEqual(applied, fourCells.map(([x, y, z]) => ({ x, y, z, before: 'air', after: 'dirt' })));
  for (const [x, y, z] of fourCells) {
    assert.equal(read([x, y, z]), 'dirt');
  }
  assert.deepEqual([read(wall.doorLower), read(wall.doorUpper)], ['air', 'air']);
  assert.deepEqual(whispers, [...(combined ? ['好，我跟着你。','嗯，跟着呢。','你指哪一格？','听得见。',
    '一根原木做不够五块木板，那四格我记着呢。','下面两格补好了，上面有块石头挡着。'] : []),
    '四格都补上了，另一处缺口留着。']);
  if (combined) {
    for (const [index, line] of ['你站到墙上，先换个目标。', '试一下普通采掘入口。'].entries()) {
      const order = collectionOrder + 2 + index;
      player.chat(line);
      const attempted = await until(() => readRows(fixture, 'game-actions.jsonl').find(row =>
        row.order === order && row.skill === 'collect'), '切换任务后检查采掘入口');
      assert.equal(attempted.result.status, 'blocked');
      assert.match(attempted.result.reason, /已验收的对象/);
      assert.deepEqual(attempted.result.changes, []);
      const { x, y, z, block } = attempted.arguments;
      assert.deepEqual([attempted.result.reasonCode, attempted.result.progress.phase, attempted.result.progress.targetCell,
        attempted.result.progress.block, attempted.result.progress.broken], ['dig_protected', 'checking_target', { x,y,z }, block, 0]);
      assert.equal(read([x,y,z]), block);
      await until(() => fixture.settled >= order, '受阻原话已处理');
    }
    assert.deepEqual(fourCells.map(read), ['dirt','dirt','dirt','dirt']);
    player.chat('验收错误依赖。');
    await until(() => traceRows().filter(row => row.kind === 'tool.exited' &&
      ['cycle','missing','incompatible'].includes(row.callId) && JSON.parse(row.output).status === 'invalid_plan').length === 3,
    '循环、缺失依赖和旧队列误用在调度前拒绝');
  }
  const trace = traceRows();
  const receipts = trace.filter(row => row.kind === 'mail.received' && row.category === 'mc.partner.action' &&
    mutations.some(action => action.work === row.body.bridgeWork && action.result.status === row.body.result?.status));
  assert.equal(trace.filter(row => row.kind === 'tool.candidate' && row.callId === 'skills').length, combined ? 1 : 0);
  assert.deepEqual(receipts.map(row => row.body.bridgeWork), mutations.map(action => action.work));
  const prepared = trace.filter(row => row.kind === 'model.prepared');
  assert.ok(prepared.length);
  const inputs = prepared.map(row => readPartnerContext(row).input);
  assert.equal(trace.filter(row => row.kind === 'tool.candidate' && row.callId === 'ask_player').length, combined ? 1 : 0);
  assert.ok(!trace.some(row => row.kind === 'tool.candidate' && row.callId === 'ask'));
  if (combined) {
    const resolved = inputs.filter((input, index) => input.questionResult?.status === 'resolved' &&
      prepared[index].step === 1);
    assert.equal(resolved.length, 1);
    assert.equal(resolved[0].questionResult.answer, '我面前的这个');
    assert.ok(!resolved[0].pendingQuestion && resolved[0].workSettlement);
    const digExit = prepared.find(row => row.step === 1 && JSON.parse(scenarioInput(row).messages.at(-1).content).questionResult?.status === 'resolved');
    assert.equal(prepared.filter(row => row.run === digExit.run && row.session === digExit.session).length, 1);
    assert.deepEqual(scenarioInput(digExit).tools.map(tool => tool.name.split('_').at(-1)), ['reply']);
    for (const [id, reason] of [['third-hit','hit_observation_limit'],['guessed-dig','pointed_block_unverified']]) {
      const rejected = trace.find(row => row.kind === 'tool.exited' && row.callId === id);
      assert.equal(JSON.parse(rejected.output).reasonCode, reason);
    }
    const plans = trace.filter(row => row.category === 'mc.partner.plan' && row.body.status === 'settled');
    assert.deepEqual(plans.map(row => row.body.layer), [1,2,1,2]);
    assert.equal(plans[2].body.nodes.length, 2);
    assert.notEqual(plans[0].body.plan, plans[2].body.plan);
    assert.equal(inputs.filter((input,index) => input.trigger === 'plan.finished' && prepared[index].step === 1).length, 2);
  }
  if (!combined) assert.equal(trace.filter(row => row.kind === 'subagent.accepted').length, 4);
}

// 只发一条指令，独立客户端分别复核原门、安装洞口和唯一完工发言。
// 步骤：确认玩家视角 → 等待实际完工 → 比对原门、库存和安装回执。
async function checkDoor(fixture, player, whispers) {
  await until(() => Math.floor(fixture.ownerEntity()?.position.x) === 7 && Math.cos(fixture.ownerEntity().yaw) > 0.99,
    '陪玩角色读到安装洞口视角');
  player.chat('把你旁边那个木门拆下来，装到我指的这个缺口');
  await until(() => whispers.includes('门已经装到你指的缺口了。'), '一句话拆装完成', 25000);
  const mutations = readRows(fixture, 'game-actions.jsonl').filter(row => ['collect', 'place'].includes(row.skill));
  assert.deepEqual(mutations.map(row => row.skill), ['collect', 'place']);
  assert.deepEqual(mutations[0].result.inventoryDelta, { oak_door: 1 });
  assert.deepEqual(mutations[0].result.changes.map(({ x, y, z, before, after }) =>
    [[x, y, z], before, after]), wall.existingDoor.map(pos => [pos, 'oak_door', 'air']));
  assert.equal(mutations[1].result.sourceMessage, 2);
  assert.equal(mutations[1].result.blocks.length, 2);
  const block = pos => player.blockAt(player.entity.position.clone().set(...pos))?.name;
  assert.ok(wall.existingDoor.every(pos => block(pos) === 'air'));
  assert.ok([wall.doorLower, wall.doorUpper].every(pos => block(pos) === 'oak_door'));
  assert.equal(block(wall.repair), 'air');
  assert.deepEqual(whispers, ['门已经装到你指的缺口了。']);
  assert.ok(!readRows(fixture, 'trace/events.jsonl').some(row => row.kind === 'ask.opened'));
}

// 独立玩家核对补墙、门洞及不同澄清路径上的世界实际状态。
// 步骤：保存发话指向并移到门洞 → 按需澄清 → 核对空门洞或批准后读回两格门。
async function checkWall(fixture, player, whispers, mode) {
  const ambiguous = mode === '--gap-ambiguous-probe';
  const askPath = mode === '--ask-direct-probe' ? 'direct' : mode.startsWith('--ask-main') ? 'main' : ambiguous ? 'local' : '';
  const manual = mode === '--ask-main-manual-probe';
  const readTrace = () => readRows(fixture, 'trace/events.jsonl');
  const block = (x, y, z) => player.blockAt(player.entity.position.clone().set(x, y, z));
  await until(() => block(2, -59, -3)?.name === 'dirt', '玩家看到原墙');
  assert.equal(block(3, -59, -3).name, 'air');
  player.chat('我们接着搭我围的墙，两层泥土，给我留扇木门。');
  await until(() => fixture.settled === 2, '保存原分工');
  fixture.send(`tp DfyGuestCheck ${wall.repairStand.join(' ')} ${ambiguous ? 0 : 180} 4`);
  await until(() => {
    const entity = fixture.ownerEntity();
    return entity && Math.abs(entity.position.x - 3.5) < 0.1 &&
      Math.cos(entity.yaw) * (ambiguous ? -1 : 1) > 0.99 && Math.abs(entity.pitch) < 0.25;
  }, '陪玩客户端收到原话位置的指定朝向');
  player.chat('看我旁边的这块缺口');
  await until(() => readTrace().some(row => row.category === 'mc.partner.event' && row.body.event.sequence === 3), '收下原话和视线');
  fixture.send(`tp DfyGuestCheck ${wall.doorStand.join(' ')}`);
  await until(() => fixture.settled === 3 && player.entity.position.x > 7 && fixture.ownerEntity()?.position.x > 7,
    '保存玩家原指向且两端均看到玩家已到门洞旁');
  player.chat('先补刚才指的缺口，门洞留空');
  if (ambiguous) {
    await until(() => whispers.includes('你指靠河的那段，还是靠木屋的那段？'), '自然追问实际送达');
    assert.equal(block(3, -59, -3).name, 'air');
    player.chat('靠河那段，留木门');
  } else if (askPath) {
    if (manual) {
      await until(() => readTrace().some(row => row.kind === 'interaction.queued' && !row.wake), '手动信箱有待收问题');
      assert.ok(!readTrace().some(row => row.kind === 'model.prepared' && row.source === 'main'));
      fixture.reviewMail();
    }
    const opened = await until(() => whispers.map(text => /回复 答复 (\d+) <回答>$/.exec(text)).find(Boolean)?.[1],
      '选定的 ASK 路线呈现问题', 45000);
    assert.equal(block(3, -59, -3).name, 'air');
    player.chat('答复 ' + opened + ' 靠河那段，留木门');
    if (manual) {
      await until(() => readTrace().some(row => row.kind === 'interaction.delivered' && row.category === 'interaction.ask.completed'),
        '手动信箱有待收原答复');
      assert.equal(readTrace().filter(row => row.kind === 'mail.dispatched' && row.category === 'interaction.ask.completed').length, 0);
      fixture.reviewMail();
    }
  }
  if (mode.startsWith('--gap-')) return checkGap(fixture, player, whispers, block, ambiguous);
  const token = await until(() => whispers.map(text => /确认 (\d+) 或 拒绝 \1$/.exec(text)).find(Boolean)?.[1],
    '原候选发出实际确认问题', 45000);
  assert.equal(block(2, -59, -3).name, 'dirt');
  assert.equal(block(3, -59, -3).name, 'dirt');
  assert.equal(block(7, -60, -3).name, 'air');
  assert.equal(block(7, -59, -3).name, 'air');
  player.chat('确认 ' + token);
  await until(() => block(7, -60, -3)?.name === 'oak_door' && block(7, -59, -3)?.name === 'oak_door',
    '玩家读回放置后的两格门', 30000);
  assert.equal(block(7, -60, -3).getProperties().half, 'lower');
  assert.equal(block(7, -59, -3).getProperties().half, 'upper');
  await until(() => fixture.settled === (askPath ? 6 : 5), '同一确认及游戏回合结算');
  await until(() => readTrace().some(row => row.kind === 'mail.received' && row.category === 'mc.partner.action' &&
    row.body.skill === 'place' && row.body.result?.status === 'done' &&
    row.body.result.blocks?.[0]?.block === 'oak_door'), '原门洞动作已退出并回到插件');
  const work = readFileSync(join(fixture.evidence, 'host/game-actions.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
  const placements = work.filter(row => row.skill === 'place');
  const order = askPath ? 5 : 4;
  assert.deepEqual(placements.map(row => [row.order, row.result.placementAttempted, row.result.placed]), [[order,true,true],[order+1,true,true]]);
  assert.equal(placements[0].result.sourceMessage, 3);
  assert.equal(placements[0].result.taskRevision, 3);
  assert.deepEqual(placements.at(-1).arguments, { block: 'oak_door', x: 7, y: -60, z: -3, sourceMessage: 4, taskRevision: 5 });
  const trace = readTrace();
  assert.ok(trace.some(row => row.kind === 'tool.exited' && row.callId === 'wrong-gap' &&
    JSON.parse(row.output).placementAttempted === false));
  const presentation = trace.find(row => row.kind === 'mail.received' && row.category === 'mc.confirmation.presentation');
  assert.ok(presentation && work.some(row => row.work === presentation.body.bridgeWork && row.result.sent));
  assert.ok(trace.some(row => row.kind === 'mail.received' && row.category === 'mc.confirmation.decision' &&
    row.body.id === presentation.body.id && row.body.status === 'approved'));
  assert.ok(trace.every(row => row.kind !== 'model.prepared' || scenarioInput(row).tools.length > 0));
  if (askPath) {
    assert.equal(whispers.filter(text => /回复 答复 \d+ <回答>$/.test(text)).length, 1);
    const ask = trace.filter(row => row.kind === 'ask.opened');
    assert.equal(ask.length, 1);
    assert.equal(ask[0].path, askPath);
    const sameEvent = row => row.eventDomain === ask[0].eventDomain && row.event === ask[0].event;
    const phaseOrder = trace.filter(row => sameEvent(row) &&
      ['event.captured', 'event.decoded', 'event.dispatched'].includes(row.kind));
    assert.deepEqual(phaseOrder.map(row => row.kind), ['event.captured', 'event.decoded', 'event.dispatched']);
    assert.equal(phaseOrder[0].type, 'mc.partner.help');
    assert.equal(phaseOrder[1].route, askPath === 'direct' ? 1 : 4);
    assert.equal(phaseOrder[2].accepted, true);
    assert.ok(trace.some(row => row.kind === 'ask.answer' && row.ask === ask[0].ask && row.accepted));
    assert.deepEqual(trace.filter(row => row.kind === 'ask.transition' && sameEvent(row)).map(row => row.phase),
      ['waiting', 'answered']);
    const delivery = trace.find(row => row.kind === 'interaction.delivered' && row.ask === ask[0].ask &&
      row.category === (askPath === 'direct' ? 'interaction.ask.result' : 'interaction.mail.reply'));
    assert.ok(delivery && delivery.body.answer === '靠河那段，留木门');
    const ack = trace.find(row => row.kind === 'interaction.acknowledged' && sameEvent(row) && row.ask === ask[0].ask);
    assert.ok(ack && ack.seq > delivery.seq && ack.disposition === 'matched' && !ack.legacyReceipt);
    assert.deepEqual([ack.mailbox, ack.message], [delivery.mailbox, delivery.message]);
    assert.ok(trace.some(row => row.kind === 'model.prepared' && row.source === 'plugin' &&
      scenarioInput(row).messages.at(-1).content.includes('靠河那段，留木门')));
    const main = trace.filter(row => row.kind === 'model.prepared' && row.source === 'main');
    const intents = trace.filter(row => row.kind === 'reply.intent' && sameEvent(row));
    if (askPath === 'direct') {
      assert.deepEqual(main, []);
      assert.deepEqual(intents, []);
    }
    else {
      const tool = main.flatMap(row => row.input.tools).find(tool => tool.name === 'ask');
      assert.deepEqual(JSON.parse(tool.parameters).properties.channel.enum, ['game']);
      assert.ok(main.some(row => row.input.tools.some(tool => tool.name === 'mail_reply')));
      assert.equal(intents.length, 1);
      const intent = intents[0];
      const received = trace.find(row => row.kind === 'mail.dispatched' && row.category === 'interaction.ask.completed' &&
        row.session === intent.session && row.run === intent.run && row.message === intent.message);
      assert.ok(received && received.seq < intent.seq && intent.seq < delivery.seq && intent.callId);
      const exits = trace.filter(row => row.kind === 'run.exited' && row.source === 'main');
      assert.equal(exits.length, 2);
      assert.ok(exits.every(row => row.diagnostic?.data.timeline.source === 'mailbox'));
    }
  }
}

// 检查原缺口已补、门洞保持空着，材料变化可以撤回正在等待的采集。
// 步骤：可选等待采集确认并提供泥土 → 核对发言和实际补墙 → 重发旧号码确认并检查没有采集。
async function checkGap(fixture, player, whispers, block, ambiguous) {
  const trace = () => readRows(fixture, 'trace/events.jsonl');
  let token;
  if (ambiguous) {
    token = await until(() => whispers.map(text => /确认 (\d+) 或 拒绝 \1$/.exec(text)).find(Boolean)?.[1],
      '库存为空时原采集候选等待确认');
    assert.equal(block(...wall.repair).name, 'air');
    fixture.send('give DfyCompanion minecraft:dirt 4');
    await until(() => fixture.materialCount('dirt') === 4, '陪玩客户端实际收到新泥土');
    player.chat('我给你土了');
    await until(() => whispers.includes('收到泥土，我补刚才那处。'), '仍在原确认期限内实际向玩家回复');
  }
  await until(() => trace().some(row => row.kind === 'tool.exited' && row.callId === 'complete' &&
    JSON.parse(row.output).task?.next === 'done'), '补墙实际退出后结束原任务');
  if (ambiguous) {
    const withdrawal = trace().find(row => row.kind === 'mail.received' && row.category === 'mc.confirmation.execution');
    assert.equal(withdrawal.body.status, 'withdrawn');
    assert.equal(withdrawal.body.execution, 'skipped');
    assert.equal(String(withdrawal.body.id), token);
    assert.equal(whispers.filter(text => /回复 答复 \d+ <回答>$/.test(text)).length, 0);
    assert.equal(whispers.filter(text => text === '你指靠河的那段，还是靠木屋的那段？').length, 1);
    assert.ok(!trace().some(row => row.kind === 'ask.opened'));
    assert.equal(whispers.filter(text => /确认 (\d+) 或 拒绝 \1$/.test(text)).length, 1);
    player.chat('确认 ' + token);
    await until(() => fixture.settled === 7, '原采集编号的迟到答复已进入新回合');
  }
  const actions = readRows(fixture, 'game-actions.jsonl');
  const placements = actions.filter(row => row.skill === 'place');
  assert.equal(placements.length, 1);
  assert.deepEqual(placements[0].result.blocks, [{ x: 3, y: -59, z: -3, block: 'dirt' }]);
  assert.equal(placements[0].result.sourceMessage, 3);
  assert.equal(placements[0].result.status, 'done');
  assert.equal(block(...wall.repair).name, 'dirt');
  assert.equal(block(...wall.doorLower).name, 'air');
  assert.equal(block(...wall.doorUpper).name, 'air');
  if (!ambiguous) assert.ok(wall.existingDoor.every(pos => block(...pos).name === 'oak_door'));
  const collections = actions.filter(row => row.skill === 'collect');
  if (ambiguous) {
    // 撤回后重提的同类请求会进入桥接复核，按原调用查实际退出和现场实效。
    const retry = trace().find(row => row.kind === 'tool.exited' && row.callId === 'approval-bypass');
    assert.ok(retry);
    assert.equal(collections.length, 1);
    const [guard] = collections;
    const exit = trace().find(row => row.kind === 'mail.received' && row.category === 'mc.partner.action' &&
      row.body.invocation === retry.invocation && row.body.bridgeWork === guard.work);
    assert.ok(exit?.body.bridgeExited && guard.exited);
    assert.deepEqual(exit.body.result, guard.result);
    assert.equal(guard.result.status, 'blocked');
    assert.equal(guard.result.reasonCode, 'approval_required');
    assert.equal(guard.result.broken, 0);
    assert.deepEqual(guard.result.changes, []);
    assert.deepEqual(guard.result.inventoryDelta, {});
    const { x, y, z } = guard.result.progress.targetCell;
    assert.equal(block(x, y, z).name, 'dirt');
    assert.equal(fixture.materialCount('dirt'), 3);
  } else assert.equal(collections.length, 0);
  assert.ok(trace().some(row => row.kind === 'tool.exited' && row.callId === 'wrong-gap' &&
    JSON.parse(row.output).placementAttempted === false));
}
module.exports = { checkPlayer };
