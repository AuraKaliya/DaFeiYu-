const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { createHash, randomBytes, randomUUID } = require('node:crypto');
const { createInterface } = require('node:readline');
const { setTimeout: sleep } = require('node:timers/promises');
const { withPaper } = require('./paper.cjs');
const { until } = require('./players.cjs');
const { launch } = require('../../plugins/mc/bridge/start.cjs');
const { course, prepareWall, prepareCourse, prepareFourCells, prepareWater, prepareLayered } = require('./playtest-arena.cjs');

// 按原版离线服的名字规则生成 UUID，只用于已经确认监听回环的体验服。
function offlineId(name) {
  const bytes = createHash('md5').update('OfflinePlayer:' + name).digest();
  bytes[6] = (bytes[6] & 15) | 48;
  bytes[8] = (bytes[8] & 63) | 128;
  return formatId(bytes);
}
// 从服务器实际写出的十六字节 UUID 得到旁证所需的同世界身份。
function formatId(bytes) {
  if (bytes.length !== 16) throw new Error('无效游戏 UUID');
  return bytes.toString('hex').replace(/^(.{8})(.{4})(.{4})(.{4})(.{12})$/, '$1-$2-$3-$4-$5');
}

// 体验场只重置预留给采石的地点和陪伴角色的物资，原玩家保留自己的背包。
function prepareScene(send) {
  for (const command of ['time set day', 'gamerule doDaylightCycle false', 'gamerule doMobSpawning false', 'effect clear DfyCompanion minecraft:mining_fatigue',
    'fill -12 -61 4 9 -61 6 minecraft:water', 'fill 19 -60 -3 23 -59 3 minecraft:stone',
    'fill 20 -58 -2 23 -58 2 minecraft:grass_block', 'fill 15 -60 -1 18 -60 2 minecraft:cobblestone',
    'tp DfyCompanion 0.5 -60 0.5', 'clear DfyCompanion', 'give DfyCompanion minecraft:iron_pickaxe 1']) send(command);
}

// 只让本次回环服先到场的玩家发出委托，同时保留这次真实交互的宿主证据。
// 步骤：等待玩家入场 → 绑定角色和观察端 → 运行宿主至接管或离场 → 收尾本次连接。
async function play(fixture, exe, pkg, mode, config, playerThinking) {
  const { send, port, server, evidence, present, running } = fixture;
  const doorProbe = mode === '--door-probe';
  const handoff = mode.startsWith('--handoff-');
  const movingHandoff = ['--handoff-moving-probe','--handoff-range-probe','--handoff-lost-probe'].includes(mode);
  const gatherHandoff = movingHandoff || mode === '--handoff-gather-probe';
  const combined = mode === '--follow-dig-fill-probe';
  const fillFour = combined || mode === '--fill-four-probe';
  const explicitOpenings = mode === '--gap-left-probe' || mode === '--gap-both-probe';
  const gapGroup = explicitOpenings || mode === '--gap-group-probe';
  const wallProbe = doorProbe || fillFour || mode === '--wall-probe' || mode.startsWith('--ask-') || mode.startsWith('--gap-');
  const withDirt = !fillFour && mode !== '--gap-ambiguous-probe';
  const root = path.join(evidence, 'host');
  const stop = path.join(root, 'stop');
  const settings = path.join(root, 'user/plugins/mc.companion');
  fs.mkdirSync(settings, { recursive: true });
  fs.writeFileSync(path.join(root, 'board.txt'), '等待本机玩家进入体验世界。\n');
  console.log('等待玩家进入 127.0.0.1:' + port + ' · ' + root);
  const stopped = () => !running() || fs.existsSync(stop);
  while (!present.size && !stopped()) await sleep(250);
  if (stopped()) return;
  const owner = present.values().next().value;
  if (owner.toLowerCase() === 'dfycompanion') throw new Error('DfyCompanion 是陪伴账号，请使用自己的玩家名');
  const channel = randomUUID(), bridgeKey = randomBytes(32).toString('hex');
  const file = path.join(evidence, 'adapter.json');
  fs.writeFileSync(path.join(settings, 'settings.json'), JSON.stringify({ channel, bridgeKey, playerThinking }));
  fs.writeFileSync(file, JSON.stringify({ host: '127.0.0.1', port, username: 'DfyCompanion', auth: 'offline',
    mode: mode === '--probe' ? 'scenario' : 'partner', owner: offlineId(owner), channel, bridgeKey, places: {
      hillside: { label: '山脚', dimension: 'overworld', x: 12, y: -60, z: 0 },
      riverside: { label: '河边', dimension: 'overworld', x: 0, y: -60, z: 0 } },
    observer: mode === '--handoff-unobserved-probe' ? undefined : { directory: path.join(server, 'plugins/DfyStoneObserver'), dimension: 'overworld',
      world: formatId(fs.readFileSync(path.join(server, 'playtest-world/uid.dat'))) } }));
  const game = await launch(file);
  if (['--handoff-late-probe','--handoff-progress-probe','--handoff-feedback-probe'].includes(mode)) {
    fixture.repeatHandoff = (job, id, order = job.order) => require('./bridge-world.cjs').query(channel, { version: 1, method: 'partner.begin',
      key: bridgeKey, id, context: { generation: job.generation, order, controller: job.controller },
      skill: 'give_item', arguments: job.arguments });
  }
  fixture.ownerEntity = () => game.bot.players[owner]?.entity;
  fixture.materialCount = material => game.bot.inventory.items().filter(item => item.name === material)
    .reduce((count, item) => count + item.count, 0);
  // 将游戏侧真实退出与原消息编号关联；日志写入失败不会改变玩家已产生的效果。
  game.bot.on('partnerSkillExit', receipt => {
    try { fs.appendFileSync(path.join(root, 'game-actions.jsonl'), JSON.stringify(receipt) + '\n'); }
    catch (error) { console.error('游戏回执写入失败：' + error.message); }
  });
  let child, exit;
  let gameEnded = false;
  const gameDone = game.done.finally(() => { gameEnded = true; });
  gameDone.catch(() => {});
  try {
    await until(() => !gameEnded && game.bot.entity && game.bot.players[owner]?.uuid === offlineId(owner),
      '角色与原玩家同时入场', 30000);
    prepareScene(send);
    if (mode === '--water-probe') prepareWater(send);
    if (mode === '--layered-follow-probe' || mode === '--layered-resupply-probe')
      prepareLayered(send, mode === '--layered-resupply-probe');
    if (mode === '--craft-probe') {
      send('tp DfyGuestCheck 0.5 -60 -2.5');
      send('kill @e[type=minecraft:item,x=-10,y=-62,z=-1,dx=8,dy=5,dz=5]');
      send('give DfyCompanion minecraft:oak_log 1');
      for (const [x, z, item, count] of [[-3.5, 0.5, 'dirt', 2], [-2.5, 1.5, 'stone', 1], [-8.5, 0.5, 'dirt', 4]])
        send(`summon minecraft:item ${x} -60 ${z} {Item:{id:"minecraft:${item}",count:${count}},PickupDelay:32767s}`);
    }
    if (handoff) {
      send('fill -3 -61 -3 7 -61 3 minecraft:dirt');
      send('fill -3 -60 -3 7 -58 3 minecraft:air');
      send('kill @e[type=minecraft:item,x=-3,y=-61,z=-3,dx=10,dy=5,dz=6]');
      send('tp DfyGuestCheck 2.5 -60 0.5');
      send('clear DfyGuestCheck minecraft:dirt');
      if (gatherHandoff) {
        if (movingHandoff) {
          send('fill 3 -61 -2 13 -61 10 minecraft:dirt');
          send('fill 3 -60 -2 13 -57 10 minecraft:air');
          if (mode === '--handoff-range-probe' || mode === '--handoff-lost-probe') {
            send('fill 13 -61 -2 38 -61 2 minecraft:stone');
            send('fill 13 -60 -2 38 -57 2 minecraft:air');
          }
        }
        send('fill -5 -62 -5 6 -61 5 minecraft:dirt');
        send('fill -6 -60 -3 -2 -58 3 minecraft:air');
        send('fill -6 -60 -3 -2 -60 3 minecraft:dirt');
        send('setblock 0 -61 0 minecraft:stone');
        send('setblock -2 -60 0 minecraft:grass_block');
        send('setblock -3 -60 0 minecraft:grass_block');
        send('effect give DfyCompanion minecraft:mining_fatigue 45 1 true');
      } else send('give DfyCompanion minecraft:dirt 70');
    }
    if (mode === '--ground-pickup-probe') {
      send('tp DfyGuestCheck 2.5 -60 -3.5');
      send('fill -8 -61 -7 2 -61 1 minecraft:dirt');
      send('fill -8 -60 -7 2 -58 1 minecraft:air');
      send('kill @e[type=minecraft:item,x=-8,y=-61,z=-7,dx=10,dy=5,dz=9]');
      for (const [x, z, item, count] of [[-4.5, 0.5, 'dirt', 3], [0.5, -5.5, 'dirt', 2], [-5.5, -4.5, 'stone', 1]])
        send(`summon minecraft:item ${x} -59.9 ${z} {Item:{id:"minecraft:${item}",count:${count}},Motion:[0d,0d,0d]}`);
    }
    if (mode === '--pickup-probe') {
      send('tp DfyCompanion 70.5 -60 0.5');
      send('tp DfyGuestCheck 72.5 -60 0.5');
      await until(() => game.bot.entity.position.x === 70.5 && [[66,-62,-6],[80,-61,5]].every(cell =>
        game.bot.blockAt(game.bot.entity.position.clone().set(...cell))), '独立取材场区块已加载');
      for (const command of ['fill 66 -62 -6 80 -61 5 minecraft:dirt', 'fill 67 -60 -5 79 -55 3 minecraft:air',
        'fill 73 -60 -2 76 -60 2 minecraft:dirt', 'setblock 73 -60 0 minecraft:grass_block',
        'tp DfyGuestCheck 72.5 -60 0.5', 'clear DfyGuestCheck', 'effect give DfyCompanion minecraft:mining_fatigue 55 1 true']) send(command);
    }
    if (wallProbe) prepareWall(send, withDirt, doorProbe || mode === '--gap-probe', doorProbe);
    if (gapGroup) {
      send('setblock 3 -60 -3 minecraft:air');
      send('tp DfyCompanion 5.5 -60 -0.5');
      send('tp DfyGuestCheck 3.5 -60 -0.5 0 4');
      if (explicitOpenings) {
        send('clear DfyCompanion minecraft:dirt');
        send('give DfyCompanion minecraft:dirt 2');
        send('tp DfyGuestCheck 5.5 -60 -0.5 180 4');
      }
    }
    if (fillFour) prepareFourCells(send, combined);
    if (mode === '--gap-materials-probe') {
      send('fill -2 -61 -2 5 -61 3 minecraft:grass_block');
      send('fill -2 -60 -2 5 -58 3 minecraft:air');
      send('setblock 2 -61 0 minecraft:dirt');
      send('tp DfyGuestCheck -1.5 -60 -1.5');
    }
    else if (!wallProbe && mode !== '--probe' && mode !== '--pickup-probe' && !handoff) {
      send('fill 3 -60 0 4 -60 0 minecraft:dirt');
      send('setblock 5 -60 1 minecraft:oak_log');
      if (mode === '--partner-probe' || mode === '--follow-probe') prepareCourse(send);
      if (mode === '--follow-probe') {
        send('tp DfyCompanion -10.5 -60 -1.5');
        send(`tp DfyGuestCheck ${course[0].join(' ')}`);
      }
    }
    if (owner === 'DfyVisitor') send('tp DfyVisitor -3.5 -60 0.5 facing 0.5 -59 0.5');
    const readyBlock = gatherHandoff ? [-2,-60,0,'grass_block'] :
      mode === '--pickup-probe' ? [73,-60,0,'grass_block'] : [15,-60,0,'cobblestone'];
    await until(() => game.bot.inventory.items().some(item => item.name === 'iron_pickaxe') &&
      game.bot.blockAt(game.bot.entity.position.clone().set(...readyBlock.slice(0, 3)))?.name === readyBlock[3], '场地与工具就绪');
    if (gatherHandoff) {
      const { fromGround } = require('../../plugins/mc/bridge/collection.cjs');
      const cells = [[-2,-60,0],[-3,-60,0]].map(cell => game.bot.entity.position.clone().set(...cell));
      await until(() => cells.every((cell,index) => {
        const b = game.bot.blockAt(cell);
        return b?.name === 'grass_block' && (index !== 0 || game.bot.canSeeBlock(b)) && fromGround(game.bot,b);
      }), '首格可见，前后两格均属于地面取材范围');
      const candidateRead = count => {
        const found = game.bot.findBlocks({ matching: game.bot.registry.blocksByName.grass_block.id, maxDistance: 16, count });
        return { count: found.length, contains: cells.map(cell => found.some(p => p.equals(cell))),
          eligible: found.filter(p => game.bot.canSeeBlock(game.bot.blockAt(p)) && fromGround(game.bot,game.bot.blockAt(p))).length };
      };
      fs.writeFileSync(path.join(evidence,'candidate-search.json'),JSON.stringify({ limited: candidateRead(96), local: candidateRead(4096) }));
    }
    if (mode === '--partner-probe') await until(() => [3, 4].every(x =>
      game.bot.blockAt(game.bot.entity.position.clone().set(x, -60, 0))?.name === 'dirt'), '客户端读回探针布置的两块泥土');
    if (wallProbe) {
      const block = (x, y) => game.bot.blockAt(game.bot.entity.position.clone().set(x,y,-3))?.name;
      await until(() => fixture.materialCount('oak_door') === (doorProbe ? 0 : 1) &&
        fixture.materialCount('dirt') === (explicitOpenings ? 2 : combined ? 0 : fillFour ? 3 : withDirt ? 8 : 0) &&
        block(2,-59) === 'dirt' && block(6,-59) === 'dirt' && block(3,-60) === (fillFour || gapGroup ? 'air' : 'dirt') && block(3,-59) === 'air',
        '原墙、缺口和建材就绪');
    }
    const args = mode === '--live' ? ['--mc-partner', config, pkg, root] : mode === '--probe' ? [pkg, root] : [pkg, root, mode];
    const review = mode === '--ask-main-manual-probe';
    child = spawn(exe, args, { windowsHide: true, stdio: [review ? 'pipe' : 'ignore', 'pipe', 'inherit'] });
    if (review) fixture.reviewMail = () => child.stdin.write('/mail review\n');
    exit = once(child, 'close');
    exit.catch(() => {});
    const lines = createInterface({ input: child.stdout });
    let departureSettled = false;
    lines.on('line', line => {
      if (line.startsWith('DFY_TRACE ') || line.startsWith('DFY_EXECUTION ')) console.log(line);
      if (line === 'DFY_LISTENING') fixture.listening = true;
      const settled = /^DFY_EVENT_SETTLED (\d+)( left)?$/.exec(line);
      if (settled) {
        fixture.settled = Number(settled[1]);
        departureSettled = Boolean(settled[2]);
      }
    });
    let absentSince;
    while (child.exitCode === null && !child.signalCode && !stopped() && !gameEnded) {
      if (present.has(owner)) absentSince = undefined;
      else {
        absentSince ??= performance.now();
        // 给真实离场事实留出本轮处理时间；失联、显式停服和模型超时仍可回收。
        if (mode === '--probe' || departureSettled || performance.now() - absentSince > 120000) break;
      }
      await sleep(100);
    }
    fs.closeSync(fs.openSync(stop, 'a'));
    const [code] = await exit;
    if (code !== 0) throw new Error('体验宿主退出码 ' + code);
  } finally {
    if (mode === '--pickup-probe' || gatherHandoff) send('effect clear DfyCompanion minecraft:mining_fatigue');
    fs.closeSync(fs.openSync(stop, 'a'));
    try { if (exit) await exit; }
    finally { game.stop(); await gameDone; }
  }
}

// 在同一次体验服生命周期内运行真实协议玩家；任一方失败都先回收本轮宿主。
async function rehearse(fixture, exe, pkg, mode, playerThinking) {
  const host = play(fixture, exe, pkg, mode, undefined, playerThinking);
  const guest = require('./playtest-check.cjs').checkPlayer(fixture, mode).catch(error => {
    fs.closeSync(fs.openSync(path.join(fixture.evidence, 'host/stop'), 'a'));
    throw error;
  });
  const results = await Promise.allSettled([host, guest]);
  for (const result of results) if (result.status === 'rejected') throw result.reason;
  if (['--gap-group-probe', '--gap-left-probe', '--gap-both-probe'].includes(mode)) {
    const feedback = JSON.parse(fs.readFileSync(path.join(fixture.evidence, 'player-feedback.json'), 'utf8'));
    const expected = playerThinking === 'low' ? { enabled: true, effort: 'low' } : { enabled: false };
    require('node:assert/strict').deepEqual(feedback.modelThinking, expected);
  }
  if (mode === '--partner-probe') {
    const assert = require('node:assert/strict');
    const seen = fs.readFileSync(path.join(fixture.evidence, 'host/model-events.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
    const turns = seen.filter(record => record.events);
    assert.ok(turns.every(turn => turn.player === offlineId('DfyGuestCheck') && turn.generation === turns[0].generation));
    const facts = seen.flatMap(record => record.events ?? []);
    assert.deepEqual(facts.map(f => f.sequence), [1,2,3,4,5,6,7,8]);
    assert.deepEqual(facts[0], { sequence: 1, type: 'player.entered' });
    assert.ok(facts.slice(1).every(f => f.type === 'player.message'));
    assert.deepEqual(facts.slice(1).map(f => f.text),
      ['大肥鱼，你在吗', '大肥鱼，帮我采俩泥土', '这回带我往前走走', '停，我来', '再走回这边', '停，我来', '模拟一次服务故障']);
    assert.deepEqual(seen.filter(r => r.check).map(r => [r.check.at(-1).text, r.stop]), [['停，我来',false], ['停，我来',true]]);
    const trace = fs.readFileSync(path.join(fixture.evidence, 'host/trace/events.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
    assert.ok(trace.some(row => row.kind === 'mail.received' && row.category === 'mc.partner.event' &&
      row.body.event.type === 'player.left' && row.body.event.sequence === 9));
    const actions = fs.readFileSync(path.join(fixture.evidence, 'host/game-actions.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
    const replies = trace.filter(row => row.kind === 'mail.received' && row.category === 'mc.partner.action' &&
      row.body.skill === 'reply' && row.body.result?.sent);
    assert.equal(replies.length, 2);
    for (const { body, seq } of replies) {
      const game = actions.find(row => row.work === body.bridgeWork);
      const accepted = trace.find(row => row.kind === 'subagent.accepted' && row.session === body.session && row.request === body.request);
      const outputs = trace.filter(row => row.kind === 'model.exited' && row.session === accepted?.session && row.run === accepted?.run);
      assert.ok(game && outputs.length >= 2);
      assert.equal(outputs.at(-1).output.message.content, game.result.text);
      assert.deepEqual(outputs.at(-1).output.message.toolCalls, []);
      assert.equal(body.result.text, game.result.text);
      if (body.order === 2) {
        assert.equal(body.invocation, 0);
        assert.equal(body.source, 'final_text');
        assert.ok(outputs.at(-1).seq < seq);
        assert.ok(outputs[0].output.message.toolCalls[0].name.endsWith('_observe'));
      } else {
        assert.equal(body.order, 7);
        const tool = trace.find(row => row.kind === 'tool.exited' && row.invocation === body.invocation);
        assert.equal(JSON.parse(tool.arguments).text, game.result.text);
        assert.equal(JSON.parse(tool.output).text, body.result.text);
      }
      assert.ok(trace.some(row => row.kind === 'mail.received' && row.category === 'mc.partner.event' &&
        row.body.generation === game.generation && row.body.event.sequence === game.order));
    }
  }
}

// 只向本机提供固定角色包，与游戏服务器共享一次体验寿命。
async function main() {
  const [exe, pkg, mode, config, liveThinking] = process.argv.slice(2);
  const playerThinking = (mode === '--live' ? liveThinking : config) ?? 'inherit';
  if (!['inherit', 'off', 'low'].includes(playerThinking)) throw new Error('玩家思考档位可选 inherit、off 或 low');
  if (!['--live', '--probe', '--partner-probe', '--follow-probe', '--wall-probe', '--ask-direct-probe', '--ask-main-probe',
    '--ask-main-manual-probe', '--gap-probe', '--gap-ambiguous-probe', '--gap-group-probe', '--gap-left-probe', '--gap-both-probe', '--gap-materials-probe', '--door-probe', '--water-probe', '--vitals-probe', '--air-episode-probe',
    '--fill-four-probe', '--follow-dig-fill-probe', '--layered-follow-probe', '--layered-resupply-probe', '--craft-probe', '--pickup-probe', '--ground-pickup-probe', '--handoff-probe', '--handoff-unobserved-probe', '--handoff-late-probe', '--handoff-progress-probe', '--handoff-feedback-probe', '--handoff-stale-feedback-probe', '--handoff-gather-probe', '--handoff-moving-probe', '--handoff-range-probe', '--handoff-lost-probe', '--handoff-other-probe'].includes(mode) ||
    (mode === '--live' && (!config || !fs.existsSync(config))))
    throw new Error('需要指定正式模型配置或固定验收模式');
  if (!exe || !pkg || !fs.existsSync(exe) || !fs.existsSync(path.join(pkg, 'dfy_mc_companion.dll')))
    throw new Error('需要已构建的体验宿主和 MC 包');
  const pack = fs.readFileSync(path.join(process.env.DFY_MC_TEST_ROOT, 'appearance/companion-1.21.4.zip'));
  const httpd = http.createServer((req, res) => {
    if (req.url !== '/companion.zip' || !['GET', 'HEAD'].includes(req.method)) { res.writeHead(404).end(); return; }
    res.writeHead(200, { 'Content-Type': 'application/zip', 'Content-Length': pack.length,
      'Access-Control-Allow-Origin': 'http://127.0.0.1:25582' });
    res.end(req.method === 'HEAD' ? undefined : pack);
  });
  httpd.listen(25581, '127.0.0.1');
  await once(httpd, 'listening');
  try {
    await withPaper(fixture => mode === '--live' ? play(fixture, exe, pkg, mode, config, playerThinking) :
      rehearse(fixture, exe, pkg, mode, playerThinking), true);
  }
  finally { httpd.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
