const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const { withPaper } = require('./paper.cjs');
const { withPlayers, until } = require('./players.cjs');
const { observeWorld } = require('../../plugins/mc/bridge/world.cjs');
const { ownerInbox } = require('../../plugins/mc/bridge/chat.cjs');
const { inspectionService, landmarks } = require('../../plugins/mc/bridge/inspection.cjs');

// 在真实双角色世界用同一技能契约采两类材料，正反两次读数都来自实际游戏。
// 步骤：接收任意原话 → 执行观察与移动 → 验证两种采集及重投 → 复核失效与私聊。
async function exercise(fixture, player, fish) {
  const world = observeWorld(fish);
  const inbox = ownerInbox(fish, world, player.player.uuid, true);
  const actions = inspectionService(fish, world, inbox, landmarks(), null, true, player.player.uuid);
  const context = { generation: world.read().generation, order: 1 };
  // 驱动一个授权技能到实际退出，并验证重投不会新开工作。
  async function invoke(skill, args) {
    const id = randomUUID();
    const query = method => actions.partner.call(method, id, context, skill, args);
    query('partner.begin');
    const receipt = await until(() => {
      const r = query('partner.read');
      return r.exited && r;
    }, skill, 25000);
    assert.deepEqual(query('partner.begin'), receipt);
    return receipt.result;
  }
  try {
    fixture.send('tp DfyCompanion 0.5 -60 0.5');
    fixture.send('tp DfyOwner -4 -60 2');
    fixture.send('clear DfyCompanion');
    fixture.send('fill 3 -60 0 4 -60 0 minecraft:dirt');
    fixture.send('setblock 5 -60 1 minecraft:oak_log');
    await until(() => fish.blockAt(fish.entity.position.clone().set(4, -60, 0))?.name === 'dirt', '泥土实际就绪');
    player.chat('给小屋备点材料吧');
    await until(() => inbox.get(context.generation, 1), '收到原话');
    assert.ok((await invoke('observe', { block: 'dirt' })).visible.length);
    assert.equal((await invoke('move', { x: 1, y: -60, z: 0 })).status, 'done');
    for (const [block, count] of [['dirt', 2], ['oak_log', 1]]) {
      const result = await invoke('collect', { block, count });
      assert.equal(result.status, 'done', JSON.stringify(result));
      assert.equal(result.broken, count);
      assert.equal(result.inventoryDelta[block], count, JSON.stringify(result));
    }
    assert.equal((await invoke('collect', { block: 'oak_log', count: 1, x: 5, y: -60, z: 1 })).status, 'blocked');
    const heard = [];
    player.on('messagestr', text => heard.push(text));
    assert.equal((await invoke('reply', { text: '材料在我背包里了。' })).sent, true);
    await until(() => heard.some(text => text.includes('材料在我背包里了')), '原玩家收到回复');
    player.chat('现在先歇会儿');
    await until(() => inbox.get(context.generation, 2), '新消息到达');
    assert.throws(() => actions.partner.call('partner.begin', randomUUID(), context, 'collect', { block: 'dirt', count: 2 }));
  } finally { await actions.close(); inbox.close(); world.close(); }
}
withPaper(fixture => withPlayers(fixture.port, (owner, fish) => exercise(fixture, owner, fish)))
  .catch(error => { console.error(error); process.exitCode = 1; });
