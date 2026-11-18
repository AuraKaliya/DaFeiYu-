const { spawn } = require('node:child_process');
const assert = require('node:assert/strict');
const { randomUUID, randomBytes } = require('node:crypto');
const { mkdirSync, writeFileSync } = require('node:fs');
const { join } = require('node:path');
const { createInterface } = require('node:readline');
const { once } = require('node:events');
const { withPaper } = require('./paper.cjs');
const { connect, until } = require('./players.cjs');
const { launch } = require('../../plugins/mc/bridge/start.cjs');
const { query } = require('./bridge-world.cjs');
const { readStream } = require('./origins.cjs');

// 让原生验证程序先确认当前聊天水位，此后才由真正的玩家协议发送新委托。
async function drive(exe, pkg, root, owner, evidence) {
  const child = spawn(exe, [pkg, root, ...process.argv.slice(4)], { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  const records = [];
  const log = createInterface({ input: child.stdout });
  let issued = false;
  let questionReady = false, questions = 0, answered = false;
  // 只在独立的玩家客户端与可信宿主都看见问题后给出一次答复。
  const answer = () => {
    if (questionReady && questions && !answered) { answered = true; owner.chat('不用了'); }
  };
  const heard = text => {
    if (text.includes('要我采一些带回来吗')) { ++questions; answer(); }
  };
  owner.on('messagestr', heard);
  log.on('line', line => {
    records.push(line);
    if (line === 'DFY_LISTENING' && !issued) { issued = true; owner.chat('去山脚看看'); }
    if (line === 'DFY_QUESTION') { questionReady = true; answer(); }
  });
  child.stderr.on('data', data => process.stderr.write(data));
  const timer = setTimeout(() => child.kill(), 45000);
  try {
    const [code] = await once(child, 'close');
    writeFileSync(join(evidence, 'host-results.json'), JSON.stringify(records, null, 2));
    if (!issued || code !== 0) throw new Error('宿主未完成真实玩家入口验收');
    if (process.argv[4] === 'decline') assert.ok(answered && questions === 1);
  } finally { clearTimeout(timer); log.close(); owner.removeListener('messagestr', heard); }
}

// 从正式游戏适配器启动双方账号，在本机私有环境将真实原话送给应用。
// 步骤：建立账号与配置 → 打开适配器并准备场地 → 预置旧消息 → 驱动宿主 → 清理游戏连接。
async function verify(fixture) {
  const owner = await connect('DfyOwner', fixture.port);
  let game;
  try {
    const channel = randomUUID(), key = randomBytes(32).toString('hex');
    const root = join(fixture.evidence, 'host');
    const settings = join(root, 'user', 'plugins', 'mc.companion');
    mkdirSync(settings, { recursive: true });
    writeFileSync(join(settings, 'settings.json'), JSON.stringify({ channel, bridgeKey: key }));
    const file = join(fixture.evidence, 'adapter.json');
    writeFileSync(file, JSON.stringify({ channel, bridgeKey: key, host: '127.0.0.1', port: fixture.port,
      auth: 'offline', username: 'DfyCompanion', owner: owner.player.uuid,
      places: { hillside: { label: '山脚', dimension: 'overworld', x: 12, y: -60, z: 0 } } }));
    game = await launch(file);
    await until(() => game.bot.player?.uuid && game.bot.entity, '适配器入场');
    fixture.send('tp DfyCompanion 0.5 -60 0.5');
    fixture.send('setblock 15 -60 0 minecraft:cobblestone');
    await until(() => Math.abs(game.bot.entity.position.x - 0.5) < 0.25 &&
      game.bot.blockAt(game.bot.entity.position.clone().set(15, -60, 0))?.name === 'cobblestone', '场地确认');
    const state = await query(channel, { version: 1, id: 'state', method: 'state.read' });
    owner.chat('开局前旧消息');
    const request = { version: 1, id: 'initial', method: 'chat.read', generation: state.state.generation, key, after: 0 };
    let recorded = false;
    for (let i = 0; i < 80 && !recorded; ++i) {
      const reply = await query(channel, request);
      recorded = reply?.inbox?.latest === 1;
      if (!recorded) await game.bot.waitForTicks(1);
    }
    if (!recorded) throw new Error('旧消息未进入游戏源');
    let digCalls = 0;
    const dig = game.bot.dig.bind(game.bot);
    game.bot.dig = (...args) => { ++digCalls; return dig(...args); };
    const first = readStream(fixture.server).length;
    await drive(process.argv[2], process.argv[3], root, owner, fixture.evidence);
    const facts = readStream(fixture.server).slice(first);
    assert.equal(digCalls, 0);
    assert.ok(!facts.some(row => row.kind === 'blockBroken' && row.actor === game.bot.player.uuid));
    if (['skip', 'tamper'].includes(process.argv[4])) assert.ok(Math.abs(game.bot.entity.position.x - 0.5) < 1);
    writeFileSync(join(fixture.evidence, 'game-observation.json'), JSON.stringify({
      actor: game.bot.player.uuid, position: game.bot.entity.position, digCalls, facts }, null, 2));
  } finally {
    owner.quit();
    if (game) { game.stop(); await game.done; }
  }
}
withPaper(verify).catch(error => { console.error(error); process.exitCode = 1; });
