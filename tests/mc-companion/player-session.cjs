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
const { deliveryProxy } = require('./delivery-proxy.cjs');

// 让原生验证程序先确认当前聊天水位，此后才由真正的玩家协议发送新委托。
async function drive(exe, pkg, root, owner, evidence) {
  const child = spawn(exe, [pkg, root, ...process.argv.slice(4)], { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  const records = [];
  const log = createInterface({ input: child.stdout });
  let issued = false;
  let questionReady = false, questions = 0, answered = false;
  // 只在独立的玩家客户端与可信宿主都看见问题后给出一次答复。
  const answer = () => {
    if (questionReady && questions && !answered) {
      answered = true;
      const variant = process.argv[4];
      owner.chat(variant === 'approve-center' ? '采 12 块带回来' : variant === 'decline' ? '不用了' : '采 16 块带回来');
    }
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
  const timer = setTimeout(() => child.kill(), process.argv[4] === 'collect-16' ? 150000 : 45000);
  try {
    const [code] = await once(child, 'close');
    writeFileSync(join(evidence, 'host-results.json'), JSON.stringify(records, null, 2));
    if (!issued || code !== 0) throw new Error('宿主未完成真实玩家入口验收');
    if (['decline', 'approve-game', 'approve-center', 'lost-send', 'lost-receipt'].includes(process.argv[4])) assert.ok(answered && questions === 1);
  } finally { clearTimeout(timer); log.close(); owner.removeListener('messagestr', heard); }
}

// 从正式游戏适配器启动双方账号，在本机私有环境将真实原话送给应用。
// 步骤：建立账号与配置 → 打开适配器并准备场地 → 预置旧消息 → 驱动宿主 → 清理游戏连接。
async function verify(fixture) {
  const owner = await connect('DfyOwner', fixture.port);
  let game, proxy;
  try {
    const channel = randomUUID(), key = randomBytes(32).toString('hex');
    const root = join(fixture.evidence, 'host');
    const settings = join(root, 'user', 'plugins', 'mc.companion');
    mkdirSync(settings, { recursive: true });
    const variant = process.argv[4];
    const fault = ['lost-send', 'lost-receipt'].includes(variant);
    const nativeChannel = fault ? randomUUID() : channel;
    writeFileSync(join(settings, 'settings.json'), JSON.stringify({ channel: nativeChannel, bridgeKey: key }));
    const file = join(fixture.evidence, 'adapter.json');
    let observer;
    if (['pickup-one', 'collect-16'].includes(variant)) {
      const marker = randomUUID();
      fixture.send('dfystonesample DfyOwner ' + marker);
      const source = await until(() => readStream(fixture.server).find(row => row.kind === 'sample' && row.request === marker), '固定同一服务器世界');
      observer = { directory: join(fixture.server, 'plugins', 'DfyStoneObserver'), world: source.world, dimension: 'overworld' };
    }
    writeFileSync(file, JSON.stringify({ channel, bridgeKey: key, host: '127.0.0.1', port: fixture.port,
      auth: 'offline', username: 'DfyCompanion', owner: owner.player.uuid, observer,
      places: { hillside: { label: '山脚', dimension: 'overworld', x: 12, y: -60, z: 0 },
        riverside: { label: '河边', dimension: 'overworld', x: 0, y: -60, z: 0 } } }));
    game = await launch(file);
    if (fault) proxy = await deliveryProxy(nativeChannel, channel, variant);
    await until(() => game.bot.player?.uuid && game.bot.entity, '适配器入场');
    fixture.send('kill @e[type=item]');
    fixture.send('tp DfyCompanion 0.5 -60 0.5');
    fixture.send('setblock 15 -60 0 minecraft:cobblestone');
    if (variant === 'collect-16') fixture.send('fill 15 -60 -1 18 -60 2 minecraft:cobblestone');
    fixture.send('clear DfyCompanion');
    if (['mine-one', 'pickup-one', 'collect-16'].includes(variant)) fixture.send('give DfyCompanion minecraft:iron_pickaxe 1');
    if (observer) fixture.send('give DfyCompanion minecraft:cobblestone 5');
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
    if (proxy) {
      const expected = ['collection.admit', 'collection.receipt'];
      if (variant === 'lost-send') expected.push('collection.admit');
      assert.deepEqual(proxy.methods, expected);
      writeFileSync(join(fixture.evidence, 'delivery-methods.json'), JSON.stringify(proxy.methods));
    }
    const facts = readStream(fixture.server).slice(first);
    const breaks = facts.filter(row => row.kind === 'blockBroken' && row.actor === game.bot.player.uuid);
    if (['mine-one', 'pickup-one'].includes(variant)) {
      assert.equal(digCalls, 1);
      assert.equal(breaks.length, 1);
      assert.equal(breaks[0].candidateAmount, 1);
      if (variant === 'pickup-one') assert.equal(game.bot.inventory.items().filter(item => item.name === 'cobblestone')
        .reduce((sum, item) => sum + item.count, 0), 6);
    } else if (variant === 'collect-16') {
      assert.equal(digCalls, 16);
      assert.equal(breaks.length, 16);
      assert.equal(game.bot.inventory.items().filter(item => item.name === 'cobblestone').reduce((sum, item) => sum + item.count, 0), 21);
    } else {
      assert.equal(digCalls, 0);
      assert.equal(breaks.length, 0);
    }
    if (['skip', 'tamper'].includes(process.argv[4])) assert.ok(Math.abs(game.bot.entity.position.x - 0.5) < 1);
    writeFileSync(join(fixture.evidence, 'game-observation.json'), JSON.stringify({
      actor: game.bot.player.uuid, position: game.bot.entity.position, digCalls, facts }, null, 2));
  } finally {
    owner.quit();
    if (proxy) await proxy.close();
    if (game) { game.stop(); await game.done; }
  }
}
withPaper(verify).catch(error => { console.error(error); process.exitCode = 1; });
