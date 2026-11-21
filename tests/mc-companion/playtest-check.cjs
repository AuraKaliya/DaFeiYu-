const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const { readFileSync, writeFileSync } = require('node:fs');
const { join } = require('node:path');
const mineflayer = require('mineflayer');
const { until } = require('./players.cjs');
const { wall, readArena } = require('./playtest-arena.cjs');

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
  await until(() => readRows(fixture, 'trace/events.jsonl').some(row => row.kind === 'tool.exited' &&
    row.callId === 'task_update' && JSON.parse(row.output).task?.next === 'done'), '实际退出后结束局内任务');
  const work = accepted.body.action.bridgeWork;
  const phases = motions();
  assert.equal(phases.filter(row => row.body.status === 'accepted').length, 1);
  assert.ok(phases.every(row => row.body.action.bridgeWork === work));
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
  const stop = trace.find(row => row.kind === 'tool.exited' && row.callId === 'stop_motion');
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
    const wallProbe = mode === '--wall-probe' || mode.startsWith('--ask-') || mode.startsWith('--gap-');
    const arena = partner ? await until(() => readArena(player, wallProbe), '独立客户端读回验收场地') : null;
    if (arena) {
      writeFileSync(join(fixture.evidence, 'arena.json'), JSON.stringify(arena, null, 2));
    }
    if (wallProbe) {
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
        chosen.map(r => JSON.stringify([r.call, r.arguments])).sort());
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
    writeFileSync(join(fixture.evidence, 'player.json'), JSON.stringify({ actor, owner: player.player.uuid, digest, chat }, null, 2));
  } finally { player.quit(); }
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
  if (askPath) {
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
  assert.deepEqual(placements.at(-1).arguments, { block: 'oak_door', x: 7, y: -60, z: -3 });
  const trace = readTrace();
  assert.ok(trace.some(row => row.kind === 'tool.exited' && row.callId === 'wrong-gap' &&
    JSON.parse(row.output).placementAttempted === false));
  const presentation = trace.find(row => row.kind === 'mail.received' && row.category === 'mc.confirmation.presentation');
  assert.ok(presentation && work.some(row => row.work === presentation.body.bridgeWork && row.result.sent));
  assert.ok(trace.some(row => row.kind === 'mail.received' && row.category === 'mc.confirmation.decision' &&
    row.body.id === presentation.body.id && row.body.status === 'approved'));
  assert.ok(trace.every(row => row.kind !== 'model.prepared' || row.input.tools.length > 0));
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
      row.input.messages.at(-1).content.includes('靠河那段，留木门')));
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
    assert.equal(whispers.filter(text => /回复 答复 \d+ <回答>$/.test(text)).length, 1);
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
  assert.ok(!actions.some(row => row.skill === 'collect'));
  assert.ok(trace().some(row => row.kind === 'tool.exited' && row.callId === 'wrong-gap' &&
    JSON.parse(row.output).placementAttempted === false));
}
module.exports = { checkPlayer };
