const assert = require('node:assert/strict');
const { test } = require('node:test');
const { EventEmitter } = require('node:events');
const { Position } = require('./test-position.cjs');
const { ownerInbox } = require('../bridge/chat.cjs');
const { chestWindows, chestSample } = require('../bridge/single-chest.cjs');
const { observationInput } = require('../bridge/partner-observe.cjs');
const { chestOrder, confirmedDelivery, transferItem, reconcileTransfer } = require('../bridge/chest-transfer.cjs');

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
  assert.equal(inbox.chestWork(generation,2),null);
  const first = {controller:'first',work:'same'};
  assert.deepEqual(inbox.chestWork(generation,2,first),first);
  assert.deepEqual(inbox.chestWork(generation,2,{controller:'second',work:'same'}),first);
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

// 模拟开箱请求失联、第一次排序回执丢失，下一次请求重查后可用同一连接。
test('开窗中断且首次服务端边界失败后可重试，并归还自己的错误类型窗口', async () => {
  const {bot,target} = chestFixture(), open = chestWindows(bot), originalOpen = bot.activateBlock;
  const stop = new AbortController();
  let fences = 0;
  bot.activateBlock = () => { stop.abort(); };
  bot.tabComplete = async () => { if (++fences === 1) throw new Error('服务端未答'); };
  await assert.rejects(open(target,stop.signal,() => {}),{code:'chest_window_unconfirmed'});
  await new Promise(resolve => setImmediate(resolve));
  let returned = false;
  bot.activateBlock = () => {
    bot.currentWindow = {type:'minecraft:hopper',close:() => { returned = true; bot.currentWindow = null; }};
    bot.emit('windowOpen',bot.currentWindow);
  };
  await assert.rejects(open(target,new AbortController().signal,() => {}),{code:'chest_window_changed'});
  assert.deepEqual([fences,returned,bot.currentWindow],[2,true,null]);
  bot.activateBlock = originalOpen;
  await open(target,new AbortController().signal,() => {});
  assert.equal(bot.listenerCount('windowOpen'),0);
});

// 控制器移交后晚到的外来窗口由新持有者处理，原监听不会关闭它。
test('迟到的窗口不可跨控制器关闭',async () => {
  const {bot,target} = chestFixture(), open = chestWindows(bot), stop = new AbortController();
  let owned = true, closed = false;
  bot.activateBlock = () => { stop.abort(); };
  bot.tabComplete = async () => { throw new Error('未取得排序回执'); };
  await assert.rejects(open(target,stop.signal,() => {},() => owned),{code:'chest_window_unconfirmed'});
  owned = false;
  bot.currentWindow = {close:() => { closed = true; }};
  bot.emit('windowOpen',bot.currentWindow);
  assert.equal(closed,false);
  bot.emit('end');
  assert.equal(bot.listenerCount('windowOpen'),0);
});

// 用独立的服务端槽位模拟普通点击和整栈转移，每次开窗取得一份新的同步副本。
function transferFixture(initial, stock, interrupt = false) {
  const { bot,target,slots } = chestFixture();
  const stone = count => count ? {name:'cobblestone',count} : null;
  slots.fill({name:'stone',count:64},0,27);
  slots[0] = stone(initial);
  slots[27] = stone(stock);
  let clicks = 0;
  const activate = bot.activateBlock;
  bot.activateBlock = () => { activate(); bot.currentWindow.slots = structuredClone(slots); };
  bot.clickWindow = async (i,button,mode) => {
    const window = bot.currentWindow;
    ++clicks;
    if (mode === 1) {
      const start = i < 27 ? 27 : 0, end = i < 27 ? 63 : 27;
      for (let dest = start; dest < end && window.slots[i]; ++dest) {
        const entry = window.slots[dest];
        if (entry && entry.name !== 'cobblestone') continue;
        const units = Math.min(window.slots[i].count,64-(entry?.count ?? 0));
        window.slots[dest] = stone((entry?.count ?? 0)+units);
        window.slots[i] = stone(window.slots[i].count-units);
      }
    } else if (button === 1 && !window.selectedItem) {
      const held = Math.ceil(window.slots[i].count/2);
      window.selectedItem = stone(held);
      window.slots[i] = stone(window.slots[i].count-held);
    } else if (button === 1) {
      window.slots[i] = stone((window.slots[i]?.count ?? 0)+1);
      window.selectedItem = stone(window.selectedItem.count-1);
    } else if (window.selectedItem && window.slots[i]?.name === window.selectedItem.name) {
      window.slots[i] = stone(window.slots[i].count+window.selectedItem.count);
      window.selectedItem = null;
    } else [window.slots[i],window.selectedItem] = [window.selectedItem ?? null,window.slots[i]];
    slots.splice(0,slots.length,...structuredClone(window.slots));
    if (interrupt) window.close();
  };
  const source = {...target,id:'observed',generation:'world',owner:'player',actor:'bot',item:'cobblestone',message:2};
  const job = {id:'work',receipt:{result:{}},abort:new AbortController()};
  const open = chestWindows(bot);
  const run = () => transferItem(bot,open,source,{count:8,sourceMessage:3},job,() => {},() => {});
  return { bot,source,job,open,run,slots,clicks:() => clicks };
}

// 复现八块委托、只能容纳三块及满箱，原有大栈也只能扣除委托数。
test('验证双方实际变化，分别结算完整、部分和零存入', async () => {
  for (const [initial,stock,stored] of [[0,8,8],[0,20,8],[61,8,3],[64,8,0],[0,4,4]]) {
    const f = transferFixture(initial,stock);
    if (stored === 8) await f.run();
    else await assert.rejects(f.run(),{code:initial === 64 ? 'chest_full' : 'chest_incomplete'});
    const proof = f.job.receipt.result.deposit;
    assert.deepEqual([proof.state,proof.stored,proof.remaining,f.slots[0]?.count,f.slots[27]?.count ?? 0],
      ['verified',stored,8-stored,initial+stored,stock-stored]);
    assert.equal(f.clicks() === 0,stored === 0);
  }
});

// 对中断的原工作重复读取同一服务端快照，净变化不一致时保留待查且不补点。
test('关闭窗口后原号查回两侧数量，复查不转移物品', async () => {
  const f = transferFixture(0,8,true);
  await assert.rejects(f.run(),{code:'chest_window_closed'});
  const proof = f.job.receipt.result.deposit;
  assert.equal(proof.state,'pending');
  assert.equal(proof.stored,undefined);
  const after = await f.open(f.source,f.job.abort.signal,window => chestSample(f.bot,window,proof.item));
  assert.equal(reconcileTransfer(proof,{...after,stock:proof.before.stock}).state,'pending');
  for (let i = 0; i < 2; ++i) {
    const checked = reconcileTransfer(proof,after);
    assert.deepEqual([checked.work,checked.state,checked.stored,checked.remaining,f.clicks()],['work','verified',8,0,1]);
  }
});

// 授权须同时匹配数量、材料和箱子，玩家单独选箱、询问或撤销均不会搬运。
test('存箱只接受匹配原指向和数量的明确委托', () => {
  const { bot,source } = transferFixture(0,8), args = {observation:'observed',item:'cobblestone',count:8,sourceMessage:3};
  for (const line of ['把这八块存进左箱','请把8块圆石放入左边的箱子']) chestOrder(bot,source,'左箱',line,null,args);
  for (const line of ['左箱','别把这八块存进左箱','把这八块存进左箱？','把这八块存进右箱','把这九块存进左箱','把8块泥土存进左箱'])
    assert.throws(() => chestOrder(bot,source,'左箱',line,null,args));
  assert.throws(() => chestOrder(bot,source,'左箱','把这八块存进左箱',{...source,cell:{x:0,y:0,z:2}},args));
  assert.throws(() => chestOrder(bot,{...source,message:3},'把这八块存进左箱','把这八块存进左箱',source,args));
  chestOrder(bot,{...source,message:3},'把这八块放进这个箱子','把这八块放进这个箱子',source,args);
});

// 从同一普通箱取到伙伴背包，比较独立槽位的实际差额和零量时的点击次数。
test('取出八块覆盖箱内不足、背包部分容量及满包', async () => {
  for (const [box,stock,expected,code] of [[8,0,8],[12,0,8],[15,0,8],[3,0,3,'chest_item_short'],
      [0,0,0,'chest_item_short'],[8,61,3,'chest_inventory_partial'],[8,64,0,'chest_inventory_full'],
      [64,0,0,'chest_split_unavailable']]) {
    const f = transferFixture(box,stock);
    f.slots.fill({name:'dirt',count:64},28,63);
    const run = transferItem(f.bot,f.open,f.source,{count:8,sourceMessage:3,destination:'companion_inventory'},f.job,() => {},() => {},true);
    if (code) await assert.rejects(run,{code}); else await run;
    const proof = f.job.receipt.result.withdrawal;
    assert.deepEqual([proof.state,proof.withdrawn,proof.remaining,f.slots[0]?.count ?? 0,f.slots[27]?.count ?? 0],
      ['verified',expected,8-expected,box-expected,stock+expected]);
    assert.equal(f.clicks() === 0,expected === 0);
  }
});

// 只认当前委托明确指定伙伴背包的取出，反向、改箱、代收、否定和问句均不提交。
test('取出绑定原指箱、材料、数量及伙伴自己的背包', () => {
  const {bot,source} = transferFixture(8,0);
  const args = {observation:'observed',sourceMessage:3,count:8,item:'cobblestone',destination:'companion_inventory'};
  const check = (line,options=args) => chestOrder(bot,source,'左箱',line,null,options,true);
  const order = '从左箱取出八块圆石，放进你的背包';
  check(order);
  check('请把左边的箱子里的8块圆石取到伙伴背包里');
  for (const line of ['左箱','把八块圆石存进左箱',order.replace('左箱','右箱'),order.replace('圆石','泥土'),
      order.replace('八','九'),order.replace('你的','我的'),'别'+order,order+'？','从左箱取出八块圆石']) assert.throws(() => check(line));
  assert.throws(() => check(order,{...args,destination:'player_inventory'}));
});

// 只有玩家本人明确要求交付时，原取出工作才产生一次交给本人的额度。
test('从原箱取三块交本人，背包原有存货不能补足八块', async () => {
  const f = transferFixture(3,17);
  const args = {observation:'observed',sourceMessage:3,count:8,item:'cobblestone',destination:'companion_inventory'};
  const grant = text => chestOrder(f.bot,f.source,'左箱',text,null,args,true);
  for (const line of ['从左箱取出八块圆石，交给我','请把左边的箱子里的8块圆石取出来给我']) assert.equal(grant(line),f.source.owner);
  for (const line of ['从左箱取出八块圆石，交给他','从右箱取出八块圆石，交给我','从左箱取出八块泥土，交给我','别从左箱取出八块圆石，交给我'])
    assert.throws(() => grant(line));
  assert.equal(grant('从左箱取出八块圆石，放进你的背包'),undefined);
  await assert.rejects(transferItem(f.bot,f.open,f.source,args,f.job,() => {},() => {},true,f.source.owner),{code:'chest_item_short'});
  const proof = f.job.receipt.result.withdrawal, give = {withdrawalWork:f.job.id,recipient:f.source.owner,item:'cobblestone',count:3};
  const offer = {recipient:f.source.owner,generation:f.source.generation,dimension:f.source.dimension,item:give.item,count:3,actor:{uuid:f.source.actor}};
  assert.deepEqual([proof.withdrawn,proof.remaining,f.slots[27].count],[3,5,20]);
  confirmedDelivery(proof,give,offer);
  for (const change of [{state:'pending'},{deliverTo:'other'},{deliverTo:undefined},{owner:'other'},{actor:'other'},{withdrawn:0},{work:'other'}])
    assert.throws(() => confirmedDelivery({...proof,...change},give,offer),{code:'chest_delivery_unconfirmed'});
  assert.throws(() => confirmedDelivery(proof,{...give,count:8},{...offer,count:8}),{code:'chest_delivery_unconfirmed'});
});
