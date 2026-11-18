const assert = require('node:assert/strict');
const { randomUUID, randomBytes } = require('node:crypto');
const { writeFileSync } = require('node:fs');
const { join } = require('node:path');
const { withPaper } = require('./paper.cjs');
const { withPlayers, until } = require('./players.cjs');
const { query } = require('./bridge-world.cjs');
const { observeWorld } = require('../../plugins/mc/bridge/world.cjs');
const { ownerInbox } = require('../../plugins/mc/bridge/chat.cjs');
const { openBridge } = require('../../plugins/mc/bridge/pipe.cjs');

// 从真正的多人聊天收取绑定玩家的消息，区分系统文本、角色自言和过期游标。
// 步骤：绑定 UUID → 发送真实对话和系统对照 → 核对增量与来源回收。
async function check(fixture, owner, fish) {
  const world = observeWorld(fish);
  const channel = randomUUID();
  const inbox = ownerInbox(fish, world, owner.player.uuid, true, 2);
  const strict = ownerInbox(fish, world, owner.player.uuid, false);
  const key = randomBytes(32).toString('hex');
  const bridge = await openBridge(channel, world, inbox, key);
  const generation = world.read().generation;
  const request = { version: 1, id: 'chat', method: 'chat.read', generation, after: 0, key };
  try {
    fixture.send('tellraw DfyCompanion {"text":"<DfyOwner> 假的采集命令"}');
    fish.chat('角色自己的话');
    owner.chat('去山脚看看');
    await until(() => inbox.get(generation, 1), '收到玩家协议原文');
    let result = (await query(channel, request)).inbox;
    assert.deepEqual(result.messages, [{ sequence: 1, text: '去山脚看看' }]);
    assert.equal(result.owner, owner.player.uuid);
    assert.equal(await query(channel, { ...request, key: '0'.repeat(64) }), null);
    assert.equal(strict.read(generation, 0).latest, 0);
    assert.equal((await query(channel, { ...request, generation: randomUUID() })).inbox, null);
    owner.chat('第二句');
    owner.chat('第三句');
    await until(() => inbox.get(generation, 3), '真实游戏聊天推进游标');
    result = (await query(channel, request)).inbox;
    assert.ok(result.gap && result.oldest === 2 && result.latest === 3 && !inbox.get(generation, 1));
    assert.deepEqual((await query(channel, { ...request, after: 2 })).inbox.messages, [{ sequence: 3, text: '第三句' }]);
    writeFileSync(join(fixture.evidence, 'owner-chat.json'), JSON.stringify(result, null, 2));
  } finally {
    strict.close();
    await bridge.close();
  }
}
withPaper(fixture => withPlayers(fixture.port, (owner, fish) => check(fixture, owner, fish)))
  .catch(error => { console.error(error); process.exitCode = 1; });
