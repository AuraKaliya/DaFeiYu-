const assert = require('node:assert/strict');
const { randomUUID, randomBytes } = require('node:crypto');
const { join } = require('node:path');
const { writeFileSync } = require('node:fs');
const { setTimeout: delay } = require('node:timers/promises');
const { withPaper } = require('./paper.cjs');
const { withPlayers, until } = require('./players.cjs');
const { readStream } = require('./origins.cjs');
const { query } = require('./bridge-world.cjs');
const { openBridge } = require('../../plugins/mc/bridge/pipe.cjs');
const { observeWorld } = require('../../plugins/mc/bridge/world.cjs');
const { ownerInbox } = require('../../plugins/mc/bridge/chat.cjs');
const { inspectionService, landmarks } = require('../../plugins/mc/bridge/inspection.cjs');

// 真实玩家发出范围明确的查看请求，核对角色实际移动、可见石料与零采掘。
// 步骤：布置目标与旁路取证 → 验证错身份及无授权拒绝 → 驱动一次查看 → 复用终态并停止。
async function verify(fixture, owner, fish) {
  fixture.send('tp DfyCompanion 0.5 -60 0.5');
  fixture.send('setblock 15 -60 0 minecraft:cobblestone');
  await until(() => Math.abs(fish.entity.position.x - 0.5) < 0.25 &&
    fish.blockAt(fish.entity.position.clone().set(15, -60, 0))?.name === 'cobblestone', '可见石料场地');
  const cursor = readStream(fixture.server).length;
  const world = observeWorld(fish);
  const generation = world.read().generation;
  const inbox = ownerInbox(fish, world, owner.player.uuid, true);
  const places = landmarks({ hillside: { label: '山脚', dimension: 'overworld', x: 12, y: -60, z: 0 } });
  const actions = inspectionService(fish, world, inbox, places);
  const channel = randomUUID();
  const key = randomBytes(32).toString('hex');
  const pipe = await openBridge(channel, world, inbox, key, actions);
  let digs = 0;
  const gameDig = fish.dig.bind(fish);
  fish.dig = (...args) => { ++digs; return gameDig(...args); };
  try {
    const request = { version: 1, method: 'inspect.begin', id: 'look', key, generation, place: 'hillside', order: 1 };
    owner.chat('先别去山脚看看');
    await until(() => inbox.get(generation, 1), '拒绝分支玩家原话');
    assert.equal(await query(channel, request), null);
    owner.chat('去山脚看看');
    await until(() => inbox.get(generation, 2), '实际委托');
    request.order = 2;
    assert.equal(await query(channel, { ...request, generation: randomUUID() }), null);
    assert.equal(await query(channel, { ...request, place: 'other' }), null);
    const started = (await query(channel, request)).action;
    assert.equal(started.phase, 'moving');
    assert.equal((await query(channel, request)).action.order, started.order);
    const poll = { version: 1, method: 'inspect.read', id: 'look-read', key, generation, order: 2 };
    let actual = started;
    const deadline = performance.now() + 20000;
    while (!actual.exited && performance.now() < deadline) {
      await delay(80);
      actual = (await query(channel, poll)).action;
    }
    assert.equal(actual.phase, 'arrived');
    assert.ok(actual.exited && actual.position.x >= 11);
    assert.deepEqual(actual.stone, { x: 15, y: -60, z: 0 });
    assert.equal(digs, 0);
    const rows = readStream(fixture.server).slice(cursor);
    assert.ok(!rows.some(row => row.kind === 'blockBroken' && row.actor === fish.player.uuid));
    assert.deepEqual((await query(channel, request)).action, actual);
    let received = 0;
    owner.on('messagestr', text => { if (text.includes('要我采一些带回来吗')) ++received; });
    const ask = { version: 1, id: 'question-transport', method: 'question.open', key, generation, order: 2, question: 'q-1' };
    const asked = (await query(channel, ask)).question;
    await until(() => received === 1, '同场玩家实际收到提问');
    assert.ok(asked.owner === owner.player.uuid && asked.id === 'q-1' && asked.sent && asked.after === 2);
    assert.deepEqual((await query(channel, ask)).question, asked);
    assert.equal(await query(channel, { ...ask, question: 'q-2' }), null);
    await owner.waitForTicks(3);
    assert.equal(received, 1);
    writeFileSync(join(fixture.evidence, 'inspect.json'), JSON.stringify({ started, actual, digs, rows, asked, received }, null, 2));
  } finally {
    fish.dig = gameDig;
    await pipe.close();
  }
}
withPaper(fixture => withPlayers(fixture.port, (owner, fish) => verify(fixture, owner, fish)))
  .catch(error => { console.error(error); process.exitCode = 1; });
