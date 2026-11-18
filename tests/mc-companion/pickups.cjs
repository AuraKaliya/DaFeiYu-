const assert = require('node:assert/strict');
const { writeFileSync } = require('node:fs');
const { join } = require('node:path');
const { withPaper } = require('./paper.cjs');
const { withPlayers, until } = require('./players.cjs');
const { prepare, readStream } = require('./origins.cjs');
const { ownPickupRange } = require('./pair-evidence.cjs');

// 从客户端当前存储栏汇总圆石，背包总量仅用作双端核对。
function stoneCount(bot) {
  return bot.inventory.items().filter(item => item.name === 'cobblestone')
    .reduce((sum, item) => sum + item.count, 0);
}

// 在夹具内将角色移动到客户端实际看见的地面物品处，等到双端拾取回执。
// 步骤：定位实例 → 安排测试位置 → 核对原实体、角色库存变化及地面余量。
async function collect(fixture, bot, item, count, ground, events) {
  const entity = await until(() => bot.entities[item.networkId], '地面物品可见');
  const { x, y, z } = item.fixtureLocation ?? entity.position;
  const before = stoneCount(bot);
  const cursor = events().length;
  fixture.send('tp ' + bot.username + ' ' + [x, y, z].join(' '));
  return until(() => {
    const rows = events().slice(cursor);
    const pickup = rows.find(row => row.kind === 'pickup' && row.actor === bot.player.uuid && row.item === item.item);
    if (!pickup) return false;
    const result = rows.find(row => row.kind === 'pickupReadback' && row.cause === pickup.seq);
    return result && result.ground === ground && pickup.remaining === ground &&
      pickup.beforeStock === before && result.afterStock === before + count && stoneCount(bot) === result.afterStock && pickup;
  }, '双端实际拾取 ' + count + '，地面剩余 ' + ground);
}

// 准备分取时仅剩一块容量的背包，使游戏自然执行部分拾取。
// 步骤：填入标准堆叠物品 → 读取整包容量并确认唯一余量。
async function limitCapacity(fixture, fish) {
  fixture.send('give DfyCompanion minecraft:cobblestone 63');
  fixture.send('give DfyCompanion minecraft:dirt 2176');
  await until(() => stoneCount(fish) === 63 && fish.inventory.emptySlotCount() === 0, '背包仅能再容纳一块圆石');
}

// 对比自然合堆和独立掉落，保存客户端拾取对象及完整服务端因果链。
// 步骤：生成两份来源 → 核对实际合堆或保持独立 → 控制两个角色接近 → 验证归属范围。
async function verifyCase(fixture, owner, fish, mode, initialStock = 0) {
  const target = await prepare(fixture, owner, fish);
  if (initialStock > 0) {
    fixture.send('give DfyCompanion minecraft:cobblestone ' + initialStock);
    await until(() => stoneCount(fish) === initialStock, '读到原有库存');
  }
  if (mode === 'split') await limitCapacity(fixture, fish);
  if (mode === 'separate') {
    fixture.send('tp DfyOwner -6.5 -60 0.5');
    await until(() => owner.entity.position.x === -6.5, '分开两份掉落的位置');
    await owner.lookAt(target.offset(-9, 0, 0.5), true);
  }
  const cursor = readStream(fixture.server).length;
  const events = () => readStream(fixture.server).slice(cursor);
  const client = [];
  const observedPickup = (who, item) => client.push({ collector: who.id, item: item.id });
  fish.on('playerCollect', observedPickup);
  try {
    await fish.dig(fish.blockAt(target));
    await until(() => events().some(row => row.kind === 'blockDrop' && row.actor === fish.player.uuid), '服务端破坏确认');
    await owner.tossStack(owner.inventory.items().find(item => item.name === 'cobblestone'));
    fixture.send('tp DfyOwner -8.5 -60 0.5');
    fixture.send('tp DfyCompanion -7.5 -60 0.5');
    const origins = await until(() => {
      const rows = events();
      const self = rows.find(row => row.kind === 'blockDrop' && row.actor === fish.player.uuid);
      const gift = rows.find(row => row.kind === 'playerDrop' && row.actor === owner.player.uuid);
      return self && gift && [self, gift].every(item => rows.some(row => row.kind === 'added' &&
        row.item === item.item && row.world === item.world && row.amount === 1)) && [self, gift];
    }, '确认原破坏与玩家丢物已经进入世界');
    const [self, gift] = origins;
    assert.notEqual(self.item, gift.item);
    let pile = self;
    if (mode !== 'separate') {
      // 夹具仅摆放已生成的两个实体，合堆、余量和拾取均由游戏本身执行。
      for (const source of origins) fixture.send('tp ' + source.item + ' 12.5 -60 0.5');
      pile = await until(() => {
        const rows = events();
        const merge = rows.find(row => row.kind === 'merge' &&
          origins.some(origin => origin.item === row.item) && origins.some(origin => origin.item === row.target));
        if (!merge) return false;
        const after = rows.find(row => row.kind === 'mergeReadback' && row.cause === merge.seq);
        const survivor = origins.find(origin => origin.item === merge.target);
        return after?.sourceRemaining === 0 && after.targetRemaining === 2 && { ...survivor, fixtureLocation: after };
      }, '两份来源自然合堆并实际在场', 20000);
    }
    const count = mode === 'full' ? 2 : 1;
    const clientStock = { before: stoneCount(fish) };
    const pickup = await collect(fixture, fish, pile, count, mode === 'split' ? 1 : 0, events);
    clientStock.after = stoneCount(fish);
    let lastCollector = fish;
    if (mode === 'split') {
      await collect(fixture, owner, pile, 1, 0, events);
      lastCollector = owner;
    }
    const rows = events();
    if (mode === 'separate') {
      assert.ok(!rows.some(row => row.kind === 'merge' && (row.item === self.item || row.target === self.item)));
    }
    // 部分入包时客户端可能只收到槽位变化，最终收走实体时再确认拾取通知。
    await until(() => client.some(record => record.collector === lastCollector.entity.id &&
      record.item === pile.networkId), '客户端看见最终拾取实体');
    const range = ownPickupRange(rows, self, gift, fish.player.uuid);
    assert.deepEqual(range, mode === 'split' ? [0, 1] : [1, 1]);
    assert.deepEqual(ownPickupRange([...rows, pickup], self, gift, fish.player.uuid), range);
    assert.equal(ownPickupRange(rows.filter(row => row.seq !== pickup.seq), self, gift, fish.player.uuid), null);
    writeFileSync(join(fixture.evidence, mode + '.json'), JSON.stringify({ mode, client, clientStock, pickup: pickup.seq,
      self: self.item, gift: gift.item, ownRange: range, rows }, null, 2));
    return { range, cursor, self, gift };
  } finally {
    fish.removeListener('playerCollect', observedPickup);
  }
}

// 在同一个受控服务器分别采集三个真实分支，每次重新准备场地。
async function main() {
  await withPaper(fixture => withPlayers(fixture.port, async (owner, fish) => {
    for (const mode of ['separate', 'full', 'split']) await verifyCase(fixture, owner, fish, mode);
  }));
}

if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
module.exports = { stoneCount, verifyCase };
