const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const { readFileSync, writeFileSync } = require('node:fs');
const { join } = require('node:path');
const mineflayer = require('mineflayer');
const { until } = require('./players.cjs');

// 用独立协议玩家走同一真人入口，核对资源包、可见角色和完整聊天回合。
// 步骤：从服务端收到资源包 → 等待收件就绪 → 交叉核对模型输入、游戏动作与真实发言。
async function checkPlayer(fixture, partner = false) {
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
    if (partner) {
      assert.deepEqual(whispers, []);
      const settled = seq => until(() => fixture.settled === seq, '处理到玩家事件 ' + seq, 50000);
      player.chat('大家叫我小叶');
      await settled(2);
      player.chat('大肥鱼，帮我采俩泥土');
      await settled(3);
      player.chat('这回带我往前走走');
      await until(() => fish.entity.position.x > 8, '原移动实际开始');
      player.chat('停，我来');
      await settled(5);
      assert.ok(fish.entity.position.x >= 31);
      player.chat('再走回这边');
      await until(() => fish.entity.position.x < 24, '第二次移动实际开始');
      player.chat('停，我来');
      await settled(7);
      const stopped = fish.entity.position.clone();
      await player.waitForTicks(12);
      assert.ok(stopped.x > 3 && stopped.distanceTo(fish.entity.position) < 0.5);
      player.chat('模拟一次服务故障');
      await settled(8);
      const receipts = readFileSync(join(fixture.evidence, 'host/game-actions.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
      const inputs = readFileSync(join(fixture.evidence, 'host/model-events.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line));
      const chosen = inputs.filter(record => record.call);
      assert.deepEqual(receipts.map(r => [r.skill, r.arguments]), chosen.map(r => [r.call, r.arguments]));
      assert.ok(receipts.some(r => r.order === 4 && r.skill === 'move' && r.result.status === 'done'));
      assert.ok(receipts.some(r => r.order === 6 && r.skill === 'move' && r.result.status === 'interrupted'));
      const sent = receipts.filter(r => r.skill === 'reply' && r.result.sent && !r.result.reused);
      assert.deepEqual(sent.map(r => r.arguments.text), chosen.filter(r => r.call === 'reply').map(r => r.arguments.text));
      assert.deepEqual(whispers, sent.map(r => r.arguments.text));
    } else {
      player.chat('去山脚看看');
      await until(() => chat.some(line => line.includes('要我采一些带回来吗')), '游戏内提问', 20000);
      player.chat('采 16 块带回来');
      await until(() => chat.some(line => line.includes('我到河边了，这次带回了 16 块圆石')), '游戏内交付', 120000);
    }
    writeFileSync(join(fixture.evidence, 'player.json'), JSON.stringify({ actor, owner: player.player.uuid, digest, chat }, null, 2));
  } finally { player.quit(); }
}
module.exports = { checkPlayer };
