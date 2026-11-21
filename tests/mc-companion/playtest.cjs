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
  for (const command of ['time set day', 'gamerule doDaylightCycle false', 'gamerule doMobSpawning false',
    'fill -12 -61 4 9 -61 6 minecraft:water', 'fill 19 -60 -3 23 -59 3 minecraft:stone',
    'fill 20 -58 -2 23 -58 2 minecraft:grass_block', 'fill 15 -60 -1 18 -60 2 minecraft:cobblestone',
    'tp DfyCompanion 0.5 -60 0.5', 'clear DfyCompanion', 'give DfyCompanion minecraft:iron_pickaxe 1']) send(command);
}

// 只让本次回环服先到场的玩家发出委托，同时保留这次真实交互的宿主证据。
// 步骤：等待玩家入场 → 绑定角色和观察端 → 运行宿主至接管或离场 → 收尾本次连接。
async function play(fixture, exe, pkg, mode, config) {
  const { send, port, server, evidence, present, running } = fixture;
  const wallProbe = mode === '--wall-probe' || mode.startsWith('--ask-');
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
  fs.writeFileSync(path.join(settings, 'settings.json'), JSON.stringify({ channel, bridgeKey }));
  fs.writeFileSync(file, JSON.stringify({ host: '127.0.0.1', port, username: 'DfyCompanion', auth: 'offline',
    mode: mode === '--probe' ? 'scenario' : 'partner', owner: offlineId(owner), channel, bridgeKey, places: {
      hillside: { label: '山脚', dimension: 'overworld', x: 12, y: -60, z: 0 },
      riverside: { label: '河边', dimension: 'overworld', x: 0, y: -60, z: 0 } },
    observer: { directory: path.join(server, 'plugins/DfyStoneObserver'), dimension: 'overworld',
      world: formatId(fs.readFileSync(path.join(server, 'playtest-world/uid.dat'))) } }));
  const game = await launch(file);
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
    if (wallProbe) {
      for (const command of ['gamerule randomTickSpeed 0', 'fill 2 -60 -3 2 -59 -3 minecraft:dirt', 'fill 6 -60 -3 6 -59 -3 minecraft:dirt',
        'setblock 3 -60 -3 minecraft:dirt', 'fill -3 -60 1 -3 -59 1 minecraft:oak_planks',
        'tp DfyGuestCheck 3.5 -60 -0.5', 'give DfyCompanion minecraft:dirt 8', 'give DfyCompanion minecraft:oak_door 1'])
        send(command);
    } else if (mode !== '--probe') {
      send('fill 3 -60 0 4 -60 0 minecraft:dirt');
      send('setblock 5 -60 1 minecraft:oak_log');
    }
    if (owner === 'DfyVisitor') send('tp DfyVisitor -3.5 -60 0.5 facing 0.5 -59 0.5');
    await until(() => game.bot.inventory.items().some(item => item.name === 'iron_pickaxe') &&
      game.bot.blockAt(game.bot.entity.position.clone().set(15, -60, 0))?.name === 'cobblestone', '山脚与工具就绪');
    if (mode === '--partner-probe') await until(() => [3, 4].every(x =>
      game.bot.blockAt(game.bot.entity.position.clone().set(x, -60, 0))?.name === 'dirt'), '客户端读回探针布置的两块泥土');
    if (wallProbe) {
      const block = (x, y) => game.bot.blockAt(game.bot.entity.position.clone().set(x,y,-3))?.name;
      await until(() => game.bot.inventory.items().some(item => item.name === 'oak_door') &&
        block(2,-59) === 'dirt' && block(6,-59) === 'dirt' && block(3,-60) === 'dirt' && block(3,-59) === 'air',
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
      if (line.startsWith('DFY_TRACE ')) console.log(line);
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
    fs.closeSync(fs.openSync(stop, 'a'));
    try { if (exit) await exit; }
    finally { game.stop(); await gameDone; }
  }
}

// 在同一次体验服生命周期内运行真实协议玩家；任一方失败都先回收本轮宿主。
async function rehearse(fixture, exe, pkg, mode) {
  const host = play(fixture, exe, pkg, mode);
  const guest = require('./playtest-check.cjs').checkPlayer(fixture, mode).catch(error => {
    fs.closeSync(fs.openSync(path.join(fixture.evidence, 'host/stop'), 'a'));
    throw error;
  });
  const results = await Promise.allSettled([host, guest]);
  for (const result of results) if (result.status === 'rejected') throw result.reason;
  if (mode === '--partner-probe') {
    const assert = require('node:assert/strict');
    const seen = fs.readFileSync(path.join(fixture.evidence, 'host/model-events.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
    const turns = seen.filter(record => record.events);
    assert.ok(turns.every(turn => turn.player === offlineId('DfyGuestCheck') && turn.generation === turns[0].generation));
    const facts = seen.flatMap(record => record.events ?? []);
    assert.deepEqual(facts.map(f => f.sequence), [1,2,3,4,5,6,7,8,9]);
    assert.deepEqual(facts[0], { sequence: 1, type: 'player.entered' });
    assert.deepEqual(facts.at(-1), { sequence: 9, type: 'player.left' });
    assert.ok(facts.slice(1,-1).every(f => f.type === 'player.message'));
    assert.deepEqual(facts.slice(1,-1).map(f => f.text),
      ['大家叫我小叶', '大肥鱼，帮我采俩泥土', '这回带我往前走走', '停，我来', '再走回这边', '停，我来', '模拟一次服务故障']);
    assert.deepEqual(seen.filter(r => r.check).map(r => [r.check.at(-1).text, r.stop]), [['停，我来',false], ['停，我来',true]]);
    const trace = fs.readFileSync(path.join(fixture.evidence, 'host/trace/events.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
    const actions = fs.readFileSync(path.join(fixture.evidence, 'host/game-actions.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
    const replies = trace.filter(row => row.kind === 'mail.received' && row.category === 'mc.partner.action' &&
      row.body.skill === 'reply' && row.body.result?.sent);
    assert.equal(replies.length, 2);
    for (const { body } of replies) {
      const game = actions.find(row => row.work === body.bridgeWork);
      const tool = trace.find(row => row.kind === 'tool.exited' && row.invocation === body.invocation);
      const model = trace.find(row => row.kind === 'model.prepared' && row.session === tool?.session && row.run === tool?.run && row.step === tool?.step);
      assert.ok(model && game);
      assert.equal(JSON.parse(tool.arguments).text, game.result.text);
      assert.equal(JSON.parse(tool.output).text, body.result.text);
      assert.ok(trace.some(row => row.kind === 'mail.received' && row.category === 'mc.partner.event' &&
        row.body.generation === game.generation && row.body.event.sequence === game.order));
    }
  }
}

// 只向本机提供固定角色包，与游戏服务器共享一次体验寿命。
async function main() {
  const [exe, pkg, mode, config] = process.argv.slice(2);
  if (!['--live', '--probe', '--partner-probe', '--wall-probe', '--ask-direct-probe', '--ask-main-probe',
    '--ask-main-manual-probe'].includes(mode) || (mode === '--live' && (!config || !fs.existsSync(config))))
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
    await withPaper(fixture => mode === '--live' ? play(fixture, exe, pkg, mode, config) : rehearse(fixture, exe, pkg, mode), true);
  }
  finally { httpd.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
