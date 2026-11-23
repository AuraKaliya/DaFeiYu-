const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const { existsSync, readFileSync, writeFileSync } = require('node:fs');
const { join } = require('node:path');
const mineflayer = require('mineflayer');
const { until } = require('./players.cjs');
const { wall, waterCourse, fourCells, layeredStands, readArena } = require('./playtest-arena.cjs');
const { readPartnerContext } = require('./partner-context.cjs');

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
    const doorProbe = mode === '--door-probe';
    const combined = mode === '--follow-dig-fill-probe';
    const resupply = mode === '--layered-resupply-probe';
    const layered = resupply || mode === '--layered-follow-probe';
    const gapGroup = mode === '--gap-group-probe';
    const wallProbe = doorProbe || mode === '--wall-probe' || mode.startsWith('--ask-') || mode.startsWith('--gap-');
    const arena = partner && !combined && !layered && !gapGroup && !['--water-probe', '--fill-four-probe', '--gap-materials-probe', '--craft-probe'].includes(mode) ?
      await until(() => readArena(player, wallProbe, doorProbe || mode === '--gap-probe'), '独立客户端读回验收场地') : null;
    if (arena) {
      writeFileSync(join(fixture.evidence, 'arena.json'), JSON.stringify(arena, null, 2));
    }
    if (combined || mode === '--fill-four-probe') {
      await checkFillFour(fixture, player, whispers, combined);
    } else if (mode === '--craft-probe') {
      await checkCraft(fixture, player, whispers);
    } else if (layered) {
      await checkLayeredFollow(fixture, player, whispers, resupply);
    } else if (doorProbe) {
      await checkDoor(fixture, player, whispers);
    } else if (mode === '--water-probe') {
      await checkWater(fixture, player, whispers);
    } else if (mode === '--gap-materials-probe') {
      await checkMaterials(fixture, player, whispers);
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
          if (row.kind !== 'model.prepared' || row.input.messages.at(-1).role !== 'user') return false;
          const work = JSON.parse(row.input.messages.at(-1).content).workSettlement;
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
    writeFileSync(join(fixture.evidence, 'player.json'), JSON.stringify({ actor, owner: player.player.uuid, digest, chat }, null, 2));
  } finally { player.quit(); }
}

// 由独立玩家核对追问回到原消息并实际补齐相隔的两处开口。
// 步骤：观察原始缺格 → 收到追问后用自然语言回答 → 核对实际方块、库存和投递。
async function checkOpeningGroup(fixture, player, whispers) {
  const names = () => [3, 7].flatMap(x => [-60, -59].map(y => player.blockAt(player.entity.position.clone().set(x, y, -3))?.name));
  await until(() => names().every(name => name === 'air') && fixture.materialCount('dirt') === 8,
    '两处竖向开口及八块泥土已就位');
  player.chat('帮我把墙上的缺口补好');
  await until(() => whispers.includes('两处缺口都要补吗？'), '收到本地追问');
  assert.deepEqual(names(), ['air', 'air', 'air', 'air']);
  player.chat('都补');
  await until(() => whispers.includes('两处缺口都补齐了。'), '收到实际补齐结果', 40000);
  assert.deepEqual(names(), ['dirt', 'dirt', 'dirt', 'dirt']);
  assert.equal(fixture.materialCount('dirt'), 4);
  assert.deepEqual(whispers, ['两处缺口都要补吗？', '两处缺口都补齐了。']);
  const rows = readRows(fixture, 'trace/events.jsonl');
  const answer = rows.find(row => row.category === 'mc.partner.question' && row.body.status === 'resolved').body;
  assert.deepEqual([answer.sourceMessage, answer.answerMessage, answer.answer], [2, 3, '都补']);
  const placed = readRows(fixture, 'game-actions.jsonl').find(row => row.skill === 'fill_target');
  assert.deepEqual([placed.exited, placed.result.sourceMessage, placed.result.changes.length], [true, 2, 4]);
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
  const settlement = JSON.parse(next.input.messages.at(-1).content);
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
  assert.deepEqual(moved.body.result.navigation, { crossings: 1, phase: 'dry' });
  assert.ok(moved.body.result.position.x < -34);
  player.chat('去封住的地方');
  const blocked = await until(() => rows().find(row => row.seq > moved.seq && row.category === 'mc.partner.action' &&
    row.body.skill === 'move'), '封闭目标给出失败回执', 40000);
  assert.equal(blocked.body.result.status, 'blocked');
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

// 用独立游戏客户端核对场景读数、合成实效和玩家收到的结果。
// 步骤：确认布置 → 发出固定原话 → 比对游戏回执、模型读回和实际发言。
async function checkCraft(fixture, player, whispers) {
  await until(() => fixture.materialCount('oak_log') === 1, '测试原木实际入包');
  const cell = player.entity.position.clone().set(3, -60, 0);
  await until(() => player.blockAt(cell)?.name === 'dirt', '独立客户端读到观察目标');
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
  const fill = mutations.at(-1);
  assert.equal(collect.arguments.source, 'ground');
  assert.deepEqual(collect.arguments.protectedCells, fourCells.map(([x,y,z]) => ({ x,y,z })));
  assert.deepEqual(collect.result.stock, { item: 'dirt', before: combined ? 1 : 3, required: 4, after: 4 });
  assert.deepEqual(collect.result.inventoryDelta, { dirt: combined ? 3 : 1 });
  assert.equal(collect.result.changes.length, combined ? 3 : 1);
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
    const digExit = prepared.find(row => row.step === 1 && JSON.parse(row.input.messages.at(-1).content).questionResult?.status === 'resolved');
    assert.equal(prepared.filter(row => row.run === digExit.run && row.session === digExit.session).length, 1);
    assert.deepEqual(digExit.input.tools.map(tool => tool.name.split('_').at(-1)), ['reply']);
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
  assert.ok(!actions.some(row => row.skill === 'collect'));
  assert.ok(trace().some(row => row.kind === 'tool.exited' && row.callId === 'wrong-gap' &&
    JSON.parse(row.output).placementAttempted === false));
}
module.exports = { checkPlayer };
