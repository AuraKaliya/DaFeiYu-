const assert = require('node:assert/strict');
const { test } = require('node:test');
const { craftPlanks } = require('../../plugins/mc/bridge/crafting.cjs');

// 生成固定配方，允许改变配方表中的单批产量和工作台要求。
function recipe(id, yieldCount, requiresTable = false) {
  const result = { id: 3, count: yieldCount, metadata: 0 };
  return { requiresTable, result, delta: [{ id, count: -1 }, result] };
}

// 在替身角色中提供可变工作台和实际库存槽位。
// 步骤：建立配方表和背包 → 记录合成调用 → 用槽位变化模拟游戏入包。
function actor(recipes, stock = 2, emptySlots = [null]) {
  const position = { distanceTo: () => 1 };
  const bot = {
    inventory: { inventoryStart: 1, inventoryEnd: 2 + emptySlots.length,
      slots: [{ type: 1, count: 64 }, { type: 1, count: stock }, ...emptySlots] },
    registry: { items: { 1: { name: 'oak_log' }, 2: { name: 'stripped_oak_log' } },
      itemsByName: { oak_planks: { id: 3, stackSize: 64 } }, blocksByName: { crafting_table: { id: 4 } } },
    entity: { position: {} }, table: { type: 4, position }, calls: [],
    // 按当前产物从夹具表返回全部配方，包括需要工作台的候选。
    recipesAll(id, meta, includeTable) { assert.deepEqual([id, meta, includeTable], [3, null, true]); return recipes; },
    findBlocks() { return [position]; }, // 在固定位置提供可复核的工作台候选。
    blockAt() { return this.table; }, // 返回当前工作台或失效后的空格。
    canSeeBlock() { return true; }, // 固定角色站位可见工作台。
    async _syncWindow(window) { assert.equal(window, this.inventory); }, // 确认替身的服务端库存已同步。
    async waitForTicks(ticks) { assert.equal(ticks, 1); }, // 替身按游戏刻步进，无需真实计时。
    // 处理一批替身合成，修改实际槽位以供生产代码读回。
    // 步骤：记录提交 → 扣除原料 → 堆叠或存入成品。
    async craft(selected, batches, table) {
      this.calls.push({ selected, batches, table });
      const slots = this.inventory.slots;
      if (--slots[1].count === 0) slots[1] = null;
      const output = slots.findIndex(stack => stack?.type === 3);
      if (output >= 0) slots[output].count += selected.result.count;
      else slots[slots.indexOf(null)] = { type: 3, metadata: 0, count: selected.result.count };
    },
  };
  return bot;
}

test('按配方表与可用原料选配方，按实际单批产量向上取整', async () => {
  const chosen = recipe(1, 5);
  const bot = actor([recipe(2, 4), recipe(1, 5, true), chosen]);
  const result = {};
  await craftPlanks(bot, { item: 'minecraft:oak_planks', count: 7 }, () => {}, result);
  assert.deepEqual(bot.calls, [{ selected: chosen, batches: 1, table: null }, { selected: chosen, batches: 1, table: null }]);
  assert.deepEqual(result.craft, { ingredient: 'oak_log', perBatch: 5, requiresTable: false, batches: 2,
    completedBatches: 2, consumed: 2, produced: 10, remaining: 0 });
  assert.deepEqual(result.inventoryDelta, { oak_log: -2, oak_planks: 10 });
});

test('缺原料、工作台或产物容量时不下发合成', async () => {
  const tableless = actor([recipe(1, 4, true)]);
  tableless.table = null;
  for (const [bot, quantity, code] of [[actor([recipe(1, 4)], 1), 5, 'insufficient_materials'],
    [tableless, 4, 'crafting_table_unavailable'], [actor([recipe(1, 4)], 2, []), 4, 'inventory_full']]) {
    const result = {};
    await assert.rejects(craftPlanks(bot, { item: 'oak_planks', count: quantity }, () => {}, result), { code });
    assert.equal(result.craftAttempted, false);
    assert.deepEqual(bot.calls, []);
  }
});

test('工作台失效后不下发剩余批次，并保留已完成批次的库存变化', async () => {
  const chosen = recipe(1, 4, true);
  const bot = actor([chosen]);
  const table = bot.table;
  const result = {};
  await assert.rejects(craftPlanks(bot, { item: 'oak_planks', count: 8 }, () => {
    if (bot.calls.length) bot.table = null;
  }, result), { code: 'craft_conditions_changed' });
  assert.deepEqual(bot.calls, [{ selected: chosen, batches: 1, table }]);
  assert.deepEqual(result.inventoryDelta, { oak_log: -1, oak_planks: 4 });
  assert.equal(result.craft.remaining, 4);
  assert.equal(result.craft.completedBatches, 1);
});

test('下发前重验原料和剩余批次，已有木板不会算作本次产出', async () => {
  const bot = actor([recipe(1, 4)], 2, [{ type: 3, metadata: 0, count: 8 }]);
  const result = {};
  await assert.rejects(craftPlanks(bot, { item: 'oak_planks', count: 8 }, () => {
    bot.inventory.slots[1].count = 1;
  }, result), { code: 'craft_conditions_changed' });
  assert.equal(result.craftAttempted, false);
  assert.equal(result.craft.remaining, 8);
  assert.equal(result.craft.produced, 0);
});

test('最后一份原料腾出的背包格可容纳产物', async () => {
  const bot = actor([recipe(1, 4)], 1, []);
  const result = {};
  await craftPlanks(bot, { item: 'oak_planks', count: 4 }, () => {}, result);
  assert.equal(result.craft.consumed, 1);
  assert.equal(result.craft.produced, 4);
});

test('服务端读回未证实产物时不把客户端预测当作入包', async () => {
  const bot = actor([recipe(1, 4)], 1);
  // 模拟服务端答复撤销客户端预测的那个槽位变化。
  bot._syncWindow = async () => { bot.inventory.slots[1] = { type: 1, count: 1 }; };
  const result = {};
  await assert.rejects(craftPlanks(bot, { item: 'oak_planks', count: 4 }, () => {}, result), { code: 'craft_unverified' });
  assert.equal(result.craftAttempted, true);
  assert.deepEqual(result.inventoryDelta, {});
  assert.equal(result.craft.produced, 0);
});

test('合成全量包先到时等待入包，同一批配方只提交一次', async () => {
  const bot = actor([recipe(1, 4)], 1);
  let reads = 0;
  // 第一次服务端快照仍在处理合成，下一刻才显示成品入包。
  bot._syncWindow = async window => { window.slots[1] = ++reads === 1 ? null : { type: 3, metadata: 0, count: 4 }; };
  const result = {};
  await craftPlanks(bot, { item: 'oak_planks', count: 4 }, () => {}, result);
  assert.equal(reads, 2);
  assert.equal(bot.calls.length, 1);
  assert.deepEqual(result.inventoryDelta, { oak_log: -1, oak_planks: 4 });
});
