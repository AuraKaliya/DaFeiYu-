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
async function play(fixture, exe, pkg) {
  const { send, port, server, evidence, present, running } = fixture;
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
    owner: offlineId(owner), channel, bridgeKey, places: {
      hillside: { label: '山脚', dimension: 'overworld', x: 12, y: -60, z: 0 },
      riverside: { label: '河边', dimension: 'overworld', x: 0, y: -60, z: 0 } },
    observer: { directory: path.join(server, 'plugins/DfyStoneObserver'), dimension: 'overworld',
      world: formatId(fs.readFileSync(path.join(server, 'playtest-world/uid.dat'))) } }));
  const game = await launch(file);
  let child, exit;
  let gameEnded = false;
  const gameDone = game.done.finally(() => { gameEnded = true; });
  gameDone.catch(() => {});
  try {
    await until(() => !gameEnded && game.bot.entity && game.bot.players[owner]?.uuid === offlineId(owner),
      '角色与原玩家同时入场', 30000);
    prepareScene(send);
    if (owner === 'DfyVisitor') send('tp DfyVisitor -3.5 -60 0.5 facing 0.5 -59 0.5');
    await until(() => game.bot.inventory.items().some(item => item.name === 'iron_pickaxe') &&
      game.bot.blockAt(game.bot.entity.position.clone().set(15, -60, 0))?.name === 'cobblestone', '山脚与工具就绪');
    child = spawn(exe, [pkg, root], { windowsHide: true, stdio: ['ignore', 'pipe', 'inherit'] });
    exit = once(child, 'close');
    exit.catch(() => {});
    const lines = createInterface({ input: child.stdout });
    lines.on('line', line => {
      if (line === 'DFY_LISTENING') game.bot.whisper(owner, '我在河边。你可以说“去山脚看看”，途中叫我“停，我来”就交给你。');
    });
    while (child.exitCode === null && !child.signalCode && present.has(owner) && !stopped() && !gameEnded) await sleep(250);
    fs.closeSync(fs.openSync(stop, 'a'));
    const [code] = await exit;
    if (code !== 0) throw new Error('体验宿主退出码 ' + code);
    if (present.has(owner) && !gameEnded && running()) {
      game.bot.whisper(owner, '这轮已结束，我先下线了。');
      await sleep(750);
    }
  } finally {
    fs.closeSync(fs.openSync(stop, 'a'));
    try { if (exit) await exit; }
    finally { game.stop(); await gameDone; }
  }
}

// 在同一次体验服生命周期内运行真实协议玩家；任一方失败都先回收本轮宿主。
async function rehearse(fixture, exe, pkg) {
  const host = play(fixture, exe, pkg);
  const guest = require('./playtest-check.cjs').checkPlayer(fixture).finally(() => {
    fs.closeSync(fs.openSync(path.join(fixture.evidence, 'host/stop'), 'a'));
  });
  const results = await Promise.allSettled([host, guest]);
  for (const result of results) if (result.status === 'rejected') throw result.reason;
}

// 只向本机提供固定角色包，与游戏服务器共享一次体验寿命。
async function main() {
  const [exe, pkg] = process.argv.slice(2);
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
    await withPaper(fixture => process.argv[4] === '--probe' ? rehearse(fixture, exe, pkg) : play(fixture, exe, pkg), true);
  }
  finally { httpd.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
