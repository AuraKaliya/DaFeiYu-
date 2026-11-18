const assert = require('node:assert/strict');
const { randomUUID, randomBytes } = require('node:crypto');
const { join } = require('node:path');
const { mkdirSync, writeFileSync } = require('node:fs');
const { execFile } = require('node:child_process');
const { promisify } = require('node:util');
const { withPaper } = require('./paper.cjs');
const { withPlayers, until } = require('./players.cjs');
const { openBridge } = require('../../plugins/mc/bridge/pipe.cjs');
const { observeWorld } = require('../../plugins/mc/bridge/world.cjs');
const { ownerInbox } = require('../../plugins/mc/bridge/chat.cjs');
const { inspectionService, landmarks } = require('../../plugins/mc/bridge/inspection.cjs');
const { readStream } = require('./origins.cjs');

// 为独立原生宿主提供实际游戏适配器及本次玩家的原消息。
// 步骤：准备回环场地 → 绑定两端私有配置 → 执行验证程序并保存双方读回 → 关闭角色入口。
async function runVisit(fixture, owner, fish, exe, pkg) {
  fixture.send('tp DfyCompanion 0.5 -60 0.5');
  fixture.send('setblock 15 -60 0 minecraft:cobblestone');
  await until(() => Math.abs(fish.entity.position.x - 0.5) < 0.25 &&
    fish.blockAt(fish.entity.position.clone().set(15, -60, 0))?.name === 'cobblestone', '原生查看夹具');
  const channel = randomUUID(), key = randomBytes(32).toString('hex');
  const world = observeWorld(fish);
  const generation = world.read().generation;
  const inbox = ownerInbox(fish, world, owner.player.uuid, true);
  const places = landmarks({ hillside: { label: '山脚', dimension: 'overworld', x: 12, y: -60, z: 0 } });
  const actions = inspectionService(fish, world, inbox, places);
  const root = join(fixture.evidence, 'host');
  const config = join(root, 'user', 'plugins', 'mc.companion');
  mkdirSync(config, { recursive: true });
  writeFileSync(join(config, 'settings.json'), JSON.stringify({ channel, bridgeKey: key }));
  const bridge = await openBridge(channel, world, inbox, key, actions);
  const cursor = readStream(fixture.server).length;
  try {
    owner.chat('去山脚看看');
    await until(() => inbox.get(generation, 1), '收到实际玩家消息');
    const { stdout } = await promisify(execFile)(exe, [pkg, root, generation], { windowsHide: true, timeout: 45000 });
    const result = JSON.parse(stdout.trim());
    assert.equal(result.phase, 'arrived');
    assert.deepEqual(result.stone, { x: 15, y: -60, z: 0 });
    assert.ok(!readStream(fixture.server).slice(cursor).some(row => row.kind === 'blockBroken' && row.actor === fish.player.uuid));
    writeFileSync(join(fixture.evidence, 'native-visit.json'), JSON.stringify(result, null, 2));
  } finally {
    await bridge.close();
  }
}

withPaper(fixture => withPlayers(fixture.port, (owner, fish) =>
  runVisit(fixture, owner, fish, process.argv[2], process.argv[3]))).catch(error => { console.error(error); process.exitCode = 1; });
