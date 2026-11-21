const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const { readFileSync, writeFileSync } = require('node:fs');
const { join } = require('node:path');
const mineflayer = require('mineflayer');
const { until } = require('./players.cjs');

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
    if (mode === '--wall-probe' || mode.startsWith('--ask-')) {
      await checkWall(fixture, player, whispers, mode);
    } else if (partner) {
      assert.deepEqual(whispers, []);
      const settled = seq => until(() => fixture.settled === seq, '处理到玩家事件 ' + seq, 50000);
      player.chat('大家叫我小叶');
      await settled(2);
      player.chat('大肥鱼，帮我采俩泥土');
      await settled(3);
      player.chat('这回带我往前走走');
      await until(() => fish.entity.position.x > 8, '原移动实际开始');
      player.chat('停，我来');
      await settled(5);
      assert.ok(fish.entity.position.x >= 31);
      player.chat('再走回这边');
      await until(() => fish.entity.position.x < 24, '第二次移动实际开始');
      player.chat('停，我来');
      await settled(7);
      const stopped = fish.entity.position.clone();
      await player.waitForTicks(12);
      assert.ok(stopped.x > 3 && stopped.distanceTo(fish.entity.position) < 0.5);
      player.chat('模拟一次服务故障');
      await settled(8);
      const receipts = readFileSync(join(fixture.evidence, 'host/game-actions.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
      const inputs = readFileSync(join(fixture.evidence, 'host/model-events.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
      const chosen = inputs.filter(record => record.call);
      assert.deepEqual(receipts.map(r => [r.skill, r.arguments]), chosen.map(r => [r.call, r.arguments]));
      assert.ok(receipts.some(r => r.order === 4 && r.skill === 'move' && r.result.status === 'done'));
      assert.ok(receipts.some(r => r.order === 6 && r.skill === 'move' && r.result.status === 'interrupted'));
      const sent = receipts.filter(r => r.skill === 'reply' && r.result.sent && !r.result.reused);
      assert.deepEqual(sent.map(r => r.arguments.text), chosen.filter(r => r.call === 'reply').map(r => r.arguments.text));
      assert.deepEqual(whispers, sent.map(r => r.arguments.text));
    } else {
      player.chat('去山脚看看');
      await until(() => chat.some(line => line.includes('要我采一些带回来吗')), '游戏内提问', 20000);
      player.chat('采 16 块带回来');
      await until(() => chat.some(line => line.includes('我到河边了，这次带回了 16 块圆石')), '游戏内交付', 120000);
    }
    writeFileSync(join(fixture.evidence, 'player.json'), JSON.stringify({ actor, owner: player.player.uuid, digest, chat }, null, 2));
  } finally { player.quit(); }
}

// 独立玩家在本地 Paper 上核对原墙、放置后的两格门和批准前的世界状态。
// 步骤：发起分工并补报位置 → 等候实际放置与询问 → 确认后读回两格门及原痕迹。
async function checkWall(fixture, player, whispers, mode) {
  const askPath = mode === '--ask-direct-probe' ? 'direct' : mode.startsWith('--ask-main') ? 'main' : '';
  const manual = mode === '--ask-main-manual-probe';
  const readTrace = () => readFileSync(join(fixture.evidence, 'host/trace/events.jsonl'), 'utf8')
    .trim().split('\n').filter(Boolean).map(JSON.parse);
  const block = (x, y, z) => player.blockAt(player.entity.position.clone().set(x, y, z));
  await until(() => block(2, -59, -3)?.name === 'dirt', '玩家看到原墙');
  assert.equal(block(3, -59, -3).name, 'air');
  player.chat('我们接着搭我围的墙，两层泥土，给我留扇木门。');
  await until(() => fixture.settled === 2, '保存原分工');
  player.chat('我在墙边上');
  if (askPath) {
    if (manual) {
      await until(() => readTrace().some(row => row.kind === 'interaction.queued' && !row.wake), '手动信箱有待收问题');
      assert.ok(!readTrace().some(row => row.kind === 'model.prepared' && row.source === 'main'));
      fixture.reviewMail();
    }
    const opened = await until(() => whispers.map(text => /回复 答复 (\d+) <回答>$/.exec(text)).filter(Boolean)[1]?.[1],
      'ASK 实际发出第二个问题', 45000);
    assert.equal(block(3, -59, -3).name, 'air');
    player.chat('答复 ' + opened + ' 靠河那段，留木门');
    if (manual) {
      await until(() => readTrace().some(row => row.kind === 'interaction.delivered' && row.category === 'interaction.ask.completed'),
        '手动信箱有待收原答复');
      assert.equal(readTrace().filter(row => row.kind === 'mail.dispatched' && row.category === 'interaction.ask.completed').length, 0);
      fixture.reviewMail();
    }
  }
  const token = await until(() => whispers.map(text => /确认 (\d+) 或 拒绝 \1$/.exec(text)).find(Boolean)?.[1],
    '原候选发出实际确认问题', 45000);
  assert.equal(block(2, -59, -3).name, 'dirt');
  assert.equal(block(3, -59, -3).name, 'dirt');
  assert.equal(block(4, -60, -3).name, 'air');
  assert.equal(block(4, -59, -3).name, 'air');
  player.chat('确认 ' + token);
  await until(() => block(4, -60, -3)?.name === 'oak_door' && block(4, -59, -3)?.name === 'oak_door',
    '玩家读回放置后的两格门', 30000);
  assert.equal(block(4, -60, -3).getProperties().half, 'lower');
  assert.equal(block(4, -59, -3).getProperties().half, 'upper');
  await until(() => fixture.settled === (askPath ? 5 : 4), '同一确认及游戏回合结算');
  const work = readFileSync(join(fixture.evidence, 'host/game-actions.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
  const placements = work.filter(row => row.skill === 'place');
  const order = askPath ? 4 : 3;
  assert.deepEqual(placements.map(row => [row.order, row.result.placementAttempted, row.result.placed]), [[order,false,false],[order,true,true],[order+1,true,true]]);
  assert.deepEqual(placements.at(-1).arguments, { block: 'oak_door', x: 4, y: -60, z: -3 });
  const trace = readTrace();
  const presentation = trace.find(row => row.kind === 'mail.received' && row.category === 'mc.confirmation.presentation');
  assert.ok(presentation && work.some(row => row.work === presentation.body.bridgeWork && row.result.sent));
  assert.ok(trace.some(row => row.kind === 'mail.received' && row.category === 'mc.confirmation.decision' &&
    row.body.id === presentation.body.id && row.body.status === 'approved'));
  assert.ok(trace.every(row => row.kind !== 'model.prepared' || row.input.tools.length > 0));
  if (askPath) {
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
module.exports = { checkPlayer };
