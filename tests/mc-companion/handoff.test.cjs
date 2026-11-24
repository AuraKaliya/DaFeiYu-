const assert = require('node:assert/strict');
const { test } = require('node:test');
const { EventEmitter } = require('node:events');
const { observeHandoff } = require('../../plugins/mc/bridge/handoff.cjs');
const { tossHandoff } = require('../../plugins/mc/bridge/give-item.cjs');
const { observationInput } = require('../../plugins/mc/bridge/partner-observe.cjs');
const { Position } = require('./test-position.cjs');

// 构造含装备格、普通栈与同名特殊栈的当前窗口，并绑定独立的配对玩家实体。
function handoffFixture() {
  const owner = '00112233-4455-6677-8899-aabbccddeeff', actor = 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee';
  const recipient = { id: 7, uuid: owner, position: new Position(3.5,-60,0.5) };
  const slots = [null, { name: 'dirt', count: 32 }, { name: 'dirt', count: 60 }, { name: 'dirt', count: 10 },
    { name: 'dirt', count: 4, components: [{ type: 'custom_name' }] }, null, { name: 'dirt', count: 15 }];
  const bot = Object.assign(new EventEmitter(), { entity: { uuid: actor, position: new Position(0.5,-60,0.5) }, entities: { 7: recipient },
    players: { owner: { uuid: owner, entity: recipient } }, game: { dimension: 'overworld' },
    registry: { itemsByName: { dirt: { stackSize: 64 } } }, inventory: { inventoryStart: 2, inventoryEnd: 6, slots,
      items() { return this.slots.slice(this.inventoryStart,this.inventoryEnd).filter(Boolean); } } });
  const args = { mode: 'handoff', recipient: owner, item: 'minecraft:dirt', count: 66 };
  return { bot, args, owner, read: () => observeHandoff(bot,owner,observationInput(args).options,'sample','world') };
}

// 核对跨堆计划按总数截取，取样保持库存原状，特殊物品、装备和副手各自保留。
test('handoff observation selects an exact amount from ordinary storage across stacks', () => {
  const f = handoffFixture();
  const initial = structuredClone(f.bot.inventory.slots);
  const out = f.read();
  assert.deepEqual(out.stock.selected, [{ slot: 2, held: 60, take: 60 }, { slot: 3, held: 10, take: 6 }]);
  assert.deepEqual([out.recipient,out.target.state,out.target.distance,out.stock.available,out.stock.missing,out.stock.excluded,out.window],
    [f.owner,'visible',3,70,0,1,'inventory']);
  assert.deepEqual(f.bot.inventory.slots, initial);
  f.args.count = 80;
  f.bot.inventory.slots[5] = undefined;
  f.bot.inventory.selectedItem = { name: 'stone' };
  assert.deepEqual([f.read().stock.status,f.read().stock.missing,f.read().stock.unknownSlots,f.read().window], ['partial',10,1,'cursor_occupied']);
  f.bot.currentWindow = {};
  delete f.bot.inventory.inventoryEnd;
  assert.deepEqual([f.read().stock.status,f.read().window], ['unavailable','container']);
});

// 重用运行编号、离开视野、原玩家不在名单与错收件人均不会把旁人选为目标。
test('handoff binds the paired player UUID and current tracked entity', () => {
  const f = handoffFixture();
  f.bot.entities[7] = { id: 7, uuid: f.bot.entity.uuid, position: f.bot.entity.position };
  assert.deepEqual(f.read().target, { state: 'not_tracked' });
  delete f.bot.players.owner;
  assert.deepEqual(f.read().target, { state: 'not_listed' });
  f.args.recipient = f.bot.entity.uuid;
  assert.throws(f.read, { code: 'handoff_recipient_unbound' });
  f.args.recipient = f.owner;
  f.args.item = 'unknown_item';
  assert.throws(f.read, { code: 'handoff_item_unknown' });
  for (const bad of [{ count: 0 }, { count: 2305 }, { recipient: 'owner' }, { radius: 3 }])
    assert.throws(() => observationInput({ ...f.args, ...bad }), { code: 'handoff_parameters' });
});

// 用可控槽位模拟游戏丢整栈和单件的实际差别，观察领取与鼠标发包互相独立。
function tossFixture() {
  const f = handoffFixture(), result = {}, clicks = [];
  const source = f.read();
  let claims = 0;
  f.bot.lookAt = async () => { setImmediate(() => f.bot.emit('physicsTick')); };
  f.bot.clickWindow = async (slot, button, mode) => {
    assert.equal(mode,4);
    clicks.push([slot,button]);
    const entry = f.bot.inventory.slots[slot];
    if (button === 1 || --entry.count === 0) f.bot.inventory.slots[slot] = null;
  };
  return { ...f, result, clicks, source, claims: () => claims,
    run: (check = () => {}, signal) => tossHandoff(f.bot,f.owner,{ ...f.args, observation: source.id },source,check,() => { ++claims; },result,signal) };
}

// 多槽位的最后一个部分栈逐件投递，特殊栈和其余数量保持原状。
test('survival toss issues exactly the requested total from ordinary slots', async () => {
  const f = tossFixture();
  await f.run();
  assert.deepEqual(f.clicks, [[2,1], ...Array.from({ length: 6 }, () => [3,0])]);
  assert.deepEqual([f.claims(), f.result.transfer.submitted, f.result.transfer.returnedClicks, f.result.inventoryDelta.dirt], [1,66,7,-66]);
  assert.equal(f.bot.inventory.slots[3].count, 4);
  assert.equal(f.bot.inventory.slots[4].components[0].type, 'custom_name');
});

// 朝向后和跨堆后都重验玩家距离；未起步与部分投出分别保留工作及实效。
test('moving recipient and interrupted clicks retain only submitted quantities', async () => {
  const before = tossFixture();
  before.bot.lookAt = async () => { before.bot.players.owner.entity.position.x += 6; setImmediate(() => before.bot.emit('physicsTick')); };
  await assert.rejects(before.run(), { code: 'handoff_recipient_distant' });
  assert.deepEqual([before.claims(),before.clicks.length,before.result.inventoryDelta], [0,0,{}]);

  const after = tossFixture();
  await assert.rejects(after.run(() => { if (after.clicks.length) throw new Error('cancelled'); }), /cancelled/);
  assert.deepEqual([after.claims(),after.result.transfer.submitted,after.result.inventoryDelta], [1,60,{ dirt: -60 }]);
  const lost = tossFixture();
  lost.bot.clickWindow = async () => { throw new Error('transport closed'); };
  await assert.rejects(lost.run(), /transport closed/);
  assert.deepEqual([lost.claims(),lost.result.transfer.submitted,lost.result.transfer.returnedClicks,lost.result.inventoryDelta], [1,60,0,{}]);
});

// 无物理刻时取消仍可退出，不留监听、投物或被占用的原观察。
test('a cancelled turn before the look packet releases the work without tossing', async () => {
  const f = tossFixture(), controller = new AbortController();
  f.bot.lookAt = async () => { setImmediate(() => controller.abort()); };
  await assert.rejects(f.run(() => { if (controller.signal.aborted) throw new Error('cancelled'); },controller.signal), /cancelled/);
  assert.deepEqual([f.claims(),f.clicks.length,f.bot.listenerCount('physicsTick')], [0,0,0]);
});
