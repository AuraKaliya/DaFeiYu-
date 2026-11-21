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
    fixture.send('setblock 12 -60 2 minecraft:oak_log');
    await until(() => fish.blockAt(fish.entity.position.clone().set(4, -60, 0))?.name === 'dirt', '泥土实际就绪');
    player.chat('给小屋备点材料吧');
    await until(() => inbox.get(context.generation, 1), '收到原话');
    assert.ok((await invoke('observe', { block: 'dirt' })).visible.length);
    assert.equal((await invoke('move', { x: 1, y: -60, z: 0 })).status, 'done');
    for (const [block, count] of [['dirt', 2], ['oak_log', 1]]) {
      const args = block === 'oak_log' ? { block, count, x: 12, y: -60, z: 2 } : { block, count };
      const result = await invoke('collect', args);
      assert.equal(result.status, 'done', JSON.stringify(result));
      assert.equal(result.broken, count);
      assert.equal(result.inventoryDelta[block], count, JSON.stringify(result));
    }
    const occupied = await invoke('place', { block: 'dirt', x: 1, y: -61, z: 0 });
    assert.equal(occupied.status, 'blocked');
    assert.equal(occupied.placementAttempted, false);
    assert.equal((await invoke('collect', { block: 'oak_log', count: 1, x: 12, y: -60, z: 2 })).status, 'blocked');
    const heard = [];
    player.on('messagestr', text => heard.push(text));
    assert.equal((await invoke('reply', { text: '材料在我背包里了。' })).sent, true);
    await until(() => heard.some(text => text.includes('材料在我背包里了')), '原玩家收到回复');
    player.chat('现在先歇会儿');
    await until(() => inbox.get(context.generation, 2), '新消息到达');
    assert.throws(() => actions.partner.call('partner.begin', randomUUID(), context, 'collect', { block: 'dirt', count: 2 }));
    const current = { ...context, order: 2 }, move = randomUUID();
    actions.partner.call('partner.begin', move, current, 'move', { x: 28, y: -60, z: -4 });
    await until(() => fish.entity.position.x > 14, '接管前确实正在移动');
    player.chat('到哪了？');
    await until(() => inbox.get(context.generation, 3), '移动期间收到玩家消息');
    const later = { ...context, order: 3 };
    const talk = randomUUID();
    const reply = actions.partner.call('partner.begin', talk, later, 'reply', { text: '还在路上。' });
    assert.ok(reply.exited && reply.result.sent);
    assert.equal(actions.partner.call('partner.read', move, current, 'move').exited, false);
    assert.equal(actions.partner.call('partner.begin', randomUUID(), later, 'observe', {}).result.status, 'done');
    assert.throws(() => actions.partner.call('partner.begin', randomUUID(), later, 'move', { place: 'player' }), /正忙/);
    actions.partner.call('partner.stop', move, current, 'move');
    await until(() => actions.partner.call('partner.read', move, current, 'move').exited, '原移动退出');
    const follow = randomUUID();
    const args = { distance: 3 };
    fixture.send('tp DfyOwner 21.5 -60 0.5');
    await until(() => fish.players.DfyOwner.entity?.position.x > 21, '跟随起点已读到');
    const followAt = (method = 'partner.read') => actions.partner.call(method, follow, later, 'follow', args);
    assert.equal(followAt('partner.begin').exited, false);
    await until(() => followAt().result.progress?.phase === 'in_range', '第一次跟上', 12000);
    const farSide = fish.entity.position.x;
    fixture.send('tp DfyOwner 9.5 -60 0.5');
    await until(() => followAt().result.progress?.phase === 'in_range' && fish.entity.position.x < 13 &&
      fish.entity.position.x < farSide - 4, '原跟随任务跨越反向目标', 12000);
    assert.equal(followAt().exited, false);
    assert.equal(actions.releaseControl(current.generation, 3).phase, 'requested');
    await until(() => actions.controlReceipt(current.generation)?.phase === 'released', '游戏确认已释放');
    assert.equal(actions.partner.call('partner.read', move, current, 'move').result.status, 'interrupted');
    assert.equal(followAt().result.status, 'interrupted');
    assert.throws(() => actions.partner.call('partner.begin', randomUUID(), current, 'move', { place: 'player' }));
  } finally { await actions.close(); inbox.close(); world.close(); }
}
withPaper(fixture => withPlayers(fixture.port, (owner, fish) => exercise(fixture, owner, fish)))
  .catch(error => { console.error(error); process.exitCode = 1; });
