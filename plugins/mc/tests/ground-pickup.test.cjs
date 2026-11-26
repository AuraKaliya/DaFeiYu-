const assert = require('node:assert/strict');
const { test } = require('node:test');
const { EventEmitter } = require('node:events');
const { Position } = require('./test-position.cjs');
const { observeDrops } = require('../bridge/dropped-items.cjs');
const { pickupDrop, pickupRemainder } = require('../bridge/pickup-drop.cjs');
const { pickupEvidence } = require('../bridge/stone-evidence.cjs');

// 用同一块自采圆石对照有效合堆、倒序合堆和倒序拾取的归属证据。
// 步骤：核对正常入包 → 分别提前两种读回 → 确认倒序时归属未知。
test('mined stone requires merge and pickup readbacks after their own events', () => {
  const at = { actor: 'fish', x: 1, y: 2, z: 3 };
  const rows = [
    { seq: 1, kind: 'blockBroken', ...at, candidateAmount: 1 },
    { seq: 2, kind: 'blockDrop', ...at, networkId: 17, item: 'self', amount: 1 },
    { seq: 3, kind: 'added', networkId: 17, item: 'self', amount: 1 },
    { seq: 4, kind: 'merge', item: 'self', amount: 1, target: 'pile', targetAmount: 1 },
    { seq: 5, kind: 'mergeReadback', cause: 4, sourceRemaining: 0, targetRemaining: 2 },
    { seq: 6, kind: 'pickup', actor: 'fish', item: 'pile', beforeStock: 0, remaining: 0 },
    { seq: 7, kind: 'pickupReadback', actor: 'fish', cause: 6, ground: 0, afterStock: 2 },
  ];
  const read = evidence => pickupEvidence(evidence, at.actor, at, new Set([17]));
  assert.equal(read(rows)?.lower, 1);
  for (const index of [3, 5]) {
    const changed = rows.map(row => ({ ...row }));
    changed[index].seq = rows[index + 1].seq;
    changed[index + 1].seq = rows[index].seq;
    changed[index + 1].cause = changed[index].seq;
    assert.equal(read(changed), null);
  }
});

// 建立可独立控制导航、实体拾取包、背包同步和时钟的地面物品现场。
function pickupFixture() {
  const bot = new EventEmitter(), result = {};
  bot._client = new EventEmitter();
  bot.entity = { id: 1, position: new Position(0.5, -60, 0.5) };
  bot.game = { dimension: 'overworld' };
  bot.registry = { itemsByName: { dirt: { stackSize: 64 } } };
  bot.inventory = { inventoryStart: 0, inventoryEnd: 2, slots: [null,null], items() { return this.slots.filter(Boolean); } };
  bot.blockAt = point => point.y < -60 ? { name: 'stone', boundingBox: 'block' } : { name: 'air', boundingBox: 'empty' };
  const uuid = '00112233-4455-6677-8899-aabbccddeeff';
  const stack = { name: 'dirt', count: 3 };
  const entity = { name: 'item', id: 5, uuid, position: new Position(3.5, -60, 0.5), getDroppedItem: () => stack };
  bot.entities = { 5: entity };
  const source = observeDrops(bot, { item: 'dirt', radius: 5 }, 'source', 'world');
  const args = { observation: 'source', entity: uuid, item: 'dirt', count: 3 };
  let at = 0, moves = 0;
  const movement = { now: () => at, pause: async ms => { at += ms; }, stop() {},
    async go(stand, allowed) { ++moves; assert.ok(allowed(stand)); bot.entity.position = stand.offset(0.5, 0, 0.5); } };
  const packet = (collector, count) => bot._client.emit('collect', { collectedEntityId: 5, collectorEntityId: collector, pickupItemCount: count });
  return { bot, entity, stack, source, args, result, movement, moves: () => moves, packet,
    run: (check = () => {}) => pickupDrop(bot, args, source, check, result, movement) };
}

// 拾取事件和库存可分包到达，只有二者都对上原堆才结算。
test('an exact pickup waits for both the selected entity and its inventory increase', async () => {
  const f = pickupFixture();
  const tick = f.movement.pause;
  let n = 0;
  f.movement.pause = async ms => {
    await tick(ms);
    if (++n === 1) { f.packet(1,3); f.bot.emit('entityGone',f.entity); delete f.bot.entities[5]; }
    if (n === 3) f.bot.inventory.slots[0] = { name: 'dirt', count: 3 };
  };
  await f.run();
  assert.deepEqual([f.moves(), f.result.progress.phase, f.result.pickup.verifiedCount, f.result.inventoryDelta],
    [1, 'received', 3, { dirt: 3 }]);
  assert.equal(f.bot._client.listenerCount('collect'), 0);
});

// 物品已变、只要求半堆、目标出界或背包已满时保持角色原位。
test('pickup revalidates the original whole stack, radius and capacity before walking', async () => {
  for (const [change, code] of [[f => { f.args.count = 2; }, 'pickup_whole_stack_required'],
    [f => { f.stack.count = 2; }, 'pickup_stack_changed'], [f => { f.entity.position.x = 9; }, 'pickup_outside_observation'],
    [f => { f.bot.inventory.slots.fill(undefined); }, 'pickup_capacity_unknown'],
    [f => { delete f.bot.inventory.inventoryStart; }, 'pickup_capacity_unknown'],
    [f => { f.bot.inventory.slots.fill({ name: 'stone', count: 64 }); }, 'pickup_insufficient_space']]) {
    const f = pickupFixture();
    change(f);
    await assert.rejects(f.run(), { code });
    assert.deepEqual([f.moves(), f.result.pickup.verifiedCount], [0, 0]);
  }
});

// 相邻未知栈会阻止起步，沿途其他栈排除自己的候选格；新物品出现时等原路退出。
test('pickup keeps adjacent stacks and the walking corridor outside its effect', async () => {
  const conflict = pickupFixture();
  const stone = { name: 'item', id: 6, position: conflict.entity.position.offset(1,0,0), getDroppedItem: () => null };
  conflict.bot.entities[6] = stone;
  await assert.rejects(conflict.run(), { code: 'pickup_other_stack_nearby' });
  assert.deepEqual([conflict.moves(), conflict.result.nearbyDrop.entityId, conflict.result.nearbyDrop.stack], [0,6,null]);

  const f = pickupFixture();
  const decoy = f.bot.entities[7] = { ...stone, id: 7, position: new Position(1.5,-60,3.5) };
  let exitWalk, stopped = 0;
  f.movement.go = async (stand, allowed) => {
    assert.ok(allowed(stand));
    assert.equal(allowed(decoy.position.floored()), false);
    await new Promise(resolve => { exitWalk = resolve; });
  };
  f.movement.stop = () => { ++stopped; exitWalk(); };
  f.movement.pause = async () => { decoy.position = f.entity.position.offset(0,0,1); };
  await assert.rejects(f.run(), { code: 'pickup_other_stack_nearby' });
  assert.deepEqual([stopped, f.result.nearbyDrop.entityId, f.result.pickup.approachAttempted], [1,7,true]);
});

// 其他实体拾走原堆时，背包里的同名物品不会替代原堆回执。
test('a concurrent inventory gain is not credited to a stack collected by someone else', async () => {
  const f = pickupFixture();
  f.movement.pause = async () => { f.bot.inventory.slots[0] = { name: 'dirt', count: 3 }; f.packet(9,3); };
  await assert.rejects(f.run(), { code: 'pickup_other_collector' });
  assert.deepEqual([f.result.pickup.observed.otherCount, f.result.pickup.verifiedCount, f.result.inventoryDelta.dirt], [3,0,3]);
});

// 中断保留已经实际拾取的部分，并等原路径退出后释放监听。
test('interruption preserves the partial pickup and joins the original navigation', async () => {
  const f = pickupFixture();
  let cancelled = false, exited = false, release;
  f.movement.go = async () => { await new Promise(resolve => { release = resolve; }); exited = true; };
  f.movement.stop = () => release();
  f.movement.pause = async () => { f.packet(1,1); f.bot.inventory.slots[0] = { name: 'dirt', count: 1 }; cancelled = true; };
  await assert.rejects(f.run(() => { if (cancelled) throw new Error('cancelled'); }), /cancelled/);
  assert.deepEqual([exited, f.result.pickup.verifiedCount, f.bot.listenerCount('entityGone')], [true,1,0]);
});

// 原堆三件先实收一件，旁边同物出现后原导航退出；恢复时只能续取原 UUID 的两件。
test('已退出的受阻工作按原包和库存续取，旁堆、未知与取消均不能替代', async () => {
  const f = pickupFixture(), decoy = {...f.entity,id:8,uuid:'00112233-4455-6677-8899-aabbccddee00'};
  let phase = 0;
  f.movement.pause = async () => {
    if (++phase === 1) {
      f.packet(1,1); f.stack.count = 2; f.bot.inventory.slots[0] = {name:'dirt',count:1};
      f.bot.entities[8] = decoy;
    }
  };
  await assert.rejects(f.run(),{code:'pickup_other_stack_nearby'});
  assert.deepEqual([f.result.pickup.verifiedCount,f.result.nearbyDrop.uuid],[1,decoy.uuid]);
  const previous = {id:'initial',receipt:{skill:'pickup_drop',generation:'world',order:2,exited:true,
    result:{...f.result,status:'blocked',reasonCode:'pickup_other_stack_nearby'}}};
  const args = {...f.args,count:2,resumeWork:'initial'};
  const recheck = () => pickupRemainder(previous,f.source,args);
  for (const [field,value] of [['exited',false],['skill','observe']]) {
    const bad = {...previous,receipt:{...previous.receipt,[field]:value}};
    assert.throws(() => pickupRemainder(bad,f.source,args),{code:field === 'exited' ? 'pickup_exit_unconfirmed' : 'pickup_remainder_unconfirmed'});
  }
  for (const status of ['done','interrupted']) {
    const bad = {...previous,receipt:{...previous.receipt,result:{...previous.receipt.result,status}}};
    assert.throws(() => pickupRemainder(bad,f.source,args),{code:status === 'done' ? 'pickup_already_completed' : 'pickup_original_interrupted'});
  }
  assert.throws(() => pickupRemainder(previous,f.source,{...args,count:3}),{code:'pickup_remainder_mismatch'});
  assert.throws(() => pickupRemainder(previous,f.source,{...args,entity:decoy.uuid}),{code:'pickup_remainder_unconfirmed'});
  previous.receipt.result.pickup.stockCurrent = false;
  assert.throws(recheck,{code:'pickup_remainder_unconfirmed'});
  previous.receipt.result.pickup.stockCurrent = true;
  assert.deepEqual(recheck(),{work:'initial',root:'initial',sourceMessage:2,originalCount:3,verifiedBefore:1,otherBefore:0});
  decoy.position = new Position(-4,-60,-4);
  const result = {};
  await assert.rejects(pickupDrop(f.bot,args,f.source,() => {},{},f.movement),{code:'pickup_whole_stack_required'});
  f.movement.pause = async () => { f.packet(1,2); f.bot.inventory.slots[0].count = 3; };
  await pickupDrop(f.bot,args,f.source,() => {},result,f.movement,recheck());
  assert.deepEqual([result.pickup.verifiedCount,result.pickup.continuation.verifiedBefore,result.inventoryDelta.dirt],[2,1,2]);
  assert.equal(f.bot.entities[8].uuid,decoy.uuid);
});
