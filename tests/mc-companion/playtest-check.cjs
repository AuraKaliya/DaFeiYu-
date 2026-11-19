const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const { readFileSync, writeFileSync } = require('node:fs');
const { join } = require('node:path');
const mineflayer = require('mineflayer');
const { until } = require('./players.cjs');

// 用独立协议玩家走同一真人入口，核对资源包、可见角色和完整聊天回合。
// 步骤：从服务端收到资源包 → 等待绑定与问候 → 根据游戏内提问作答 → 核对带回后叫停。
async function checkPlayer(fixture, partner = false) {
  const player = mineflayer.createBot({ host: '127.0.0.1', port: fixture.port, username: 'DfyGuestCheck',
    auth: 'offline', version: '1.21.4' });
  let pack;
  const chat = [];
  player._client.on('add_resource_pack', data => { pack = data; });
  player.on('messagestr', line => { chat.push(line); });
  try {
    await until(() => pack, '真实玩家协议收到体验服资源包', 20000);
    assert.equal(pack.url, 'http://127.0.0.1:25581/companion.zip');
    const response = await fetch(pack.url);
    assert.equal(response.status, 200);
    const digest = createHash('sha1').update(Buffer.from(await response.arrayBuffer())).digest('hex');
    assert.equal(pack.hash, digest);
    await until(() => chat.some(line => line.includes('我在河边')), '玩家收到角色就绪问候', 45000);
    const fish = player.players.DfyCompanion;
    assert.ok(fish?.uuid && fish.uuid !== player.player.uuid && fish.entity);
    const actor = fish.uuid;
    if (partner) {
      player.chat('大家叫我小叶');
      await until(() => chat.some(line => line.includes('小叶，我记住了')), '自由原话的最终回复');
      player.chat('大肥鱼，帮我采俩泥土');
      await until(() => chat.some(line => line.includes('背包里多了两块泥土')), '通用工具的游戏结果', 50000);
      await player.waitForTicks(12);
      assert.equal(chat.filter(line => line.includes('背包里多了两块泥土')).length, 1);
      player.chat('这回带我往前走走');
      await until(() => fish.entity.position.x > 8, '原移动实际开始');
      player.chat('改主意了，先站这儿');
      await until(() => chat.some(line => line.includes('好，我停在这儿')), '新原话中断旧委托');
      const stopped = fish.entity.position.clone();
      await player.waitForTicks(12);
      assert.ok(stopped.x < 30 && stopped.distanceTo(fish.entity.position) < 0.5);
      player.chat('模拟一次服务故障');
      await until(() => chat.some(line => line.includes('没能完成模型回合')), '模型故障能在游戏内看到');
      const receipts = readFileSync(join(fixture.evidence, 'host/game-actions.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
      const collection = receipts.find(r => r.skill === 'collect');
      assert.ok(collection?.order === 2 && collection.result.broken === 2 && collection.result.inventoryDelta.dirt === 2);
      assert.ok(receipts.some(r => r.order === 3 && r.skill === 'move' && r.result.status !== 'done'));
    } else {
      player.chat('去山脚看看');
      await until(() => chat.some(line => line.includes('要我采一些带回来吗')), '游戏内提问', 20000);
      player.chat('采 16 块带回来');
      await until(() => chat.some(line => line.includes('我到河边了，这次带回了 16 块圆石')), '游戏内交付', 120000);
    }
    player.chat('停，我来');
    await until(() => chat.some(line => line.includes('这轮已结束')), '原工作实际退出', 10000);
    writeFileSync(join(fixture.evidence, 'player.json'), JSON.stringify({ actor, owner: player.player.uuid, digest, chat }, null, 2));
  } finally { player.quit(); }
}
module.exports = { checkPlayer };
