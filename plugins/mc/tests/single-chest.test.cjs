const assert = require('node:assert/strict');
const { test } = require('node:test');
const { EventEmitter } = require('node:events');
const { Position } = require('./test-position.cjs');
const { ownerInbox } = require('../bridge/chat.cjs');
const { chestWindows, chestSample } = require('../bridge/single-chest.cjs');
const { observationInput } = require('../bridge/partner-observe.cjs');

// 提供位于两个普通箱子之间的玩家、同步窗口及独立的伙伴库存槽。
function chestFixture() {
  const bot = new EventEmitter(), owner = '00112233-4455-6677-8899-aabbccddeeff';
  bot._client = new EventEmitter();
  bot.entity = { position: new Position(0.5,0,0.5), eyeHeight: 1.62 };
  bot.players = { owner: { uuid: owner, entity: { ...bot.entity, yaw: 0, pitch: -0.5 } } };
  bot.game = { dimension: 'overworld' };
  bot.inventory = {};
  bot.getControlState = () => false;
  bot.canSeeBlock = () => true;
  bot.registry = { itemsByName: { cobblestone: { stackSize: 64 } } };
  bot.blockAt = p => ({ position:p, name: p.x === 0 && p.y === 0 && [-2,2].includes(p.z) ? 'chest' : 'air',
    getProperties: () => ({type:'single'}) });
  const target = { dimension:'overworld',cell:{x:0,y:0,z:-2} };
  const slots = Array(63).fill(null);
  slots[0] = { name:'cobblestone',count:61 };
  slots[27] = { name:'cobblestone',count:8 };
  bot.activateBlock = () => {
    bot.currentWindow = { type:'minecraft:generic_9x3',inventoryStart:27,inventoryEnd:63,slots,
      close: () => { bot.currentWindow = null; } };
    bot.emit('windowOpen',bot.currentWindow);
  };
  return { bot,owner,target,slots };
}

// 收件之后转头、换世界或再看双箱，都不能把旧原话改绑到另一箱。
test('指箱回执只接受原消息命中的普通单箱', () => {
  const { bot,owner,target } = chestFixture();
  let generation = 'first';
  const inbox = ownerInbox(bot,{read:() => ({generation,dimension:'overworld'})},owner,true,8,undefined,true);
  inbox.read(generation,0);
  const tell = () => bot._client.emit('playerChat',{sender:owner,plainMessage:'指这个箱子'});
  tell();
  bot.players.owner.entity.yaw = Math.PI;
  tell();
  assert.deepEqual(inbox.pointedChest(generation,2).cell,target.cell);
  assert.equal(inbox.pointedChest(generation,3).cell.z,2);
  const blockAt = bot.blockAt;
  bot.blockAt = p => ({...blockAt(p),getProperties:() => ({type:'left'})});
  tell();
  assert.equal(inbox.pointedChest(generation,4),null);
  generation = 'second';
  assert.equal(inbox.pointedChest('first',2),null);
  inbox.close();
});

// 分开核对箱内与伙伴槽位，指定期外参数和外来窗口均不能开始观察。
test('单箱只读报告容量和覆盖并释放原窗口', async () => {
  const { bot,target,slots } = chestFixture(), open = chestWindows(bot), signal = new AbortController().signal;
  slots[2] = undefined;
  const sample = await open(target,signal,window => chestSample(bot,window,'cobblestone'));
  assert.deepEqual([sample.container.status,sample.container.knownSpace,sample.container.stored,sample.stock.stored],
    ['partial',25*64+3,61,8]);
  assert.equal(bot.currentWindow,null);
  assert.throws(() => observationInput({mode:'chest',message:2,item:'cobblestone',x:0}),{code:'chest_parameters'});
  bot.currentWindow = { type:'minecraft:hopper' };
  await assert.rejects(open(target,signal,() => {}),{code:'chest_window_busy'});
  assert.equal(bot.currentWindow.type,'minecraft:hopper');
});
