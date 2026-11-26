// 汇总同一窗口内指定范围的普通物品、合堆容量与缺读格数。
// 步骤：核对槽位范围 → 按堆叠上限累计库存及容量 → 标明实际覆盖。
function slotRange(slots, start, end, item, size) {
  if (!Number.isInteger(size) || size <= 0 || !Array.isArray(slots) || !Number.isInteger(start) ||
      !Number.isInteger(end) || start < 0 || end <= start || end > slots.length) return { status: 'unavailable' };

  let emptySlots = 0, mergeSpace = 0, unknownSlots = 0, stored = 0;
  for (let index = start; index < end; ++index) {
    const stack = slots[index];
    if (stack === null) { ++emptySlots; continue; }
    if (!stack?.name) { ++unknownSlots; continue; }
    if (stack.name !== item) continue;
    if (stack.nbt || stack.metadata || stack.components?.length || stack.removedComponents?.length ||
        !Number.isInteger(stack.count) || stack.count <= 0 || stack.count > size) { ++unknownSlots; continue; }
    mergeSpace += size - stack.count;
    stored += stack.count;
  }
  return { status: unknownSlots ? 'partial' : 'sampled', slots: end-start, stackSize: size,
    emptySlots, mergeSpace, knownSpace: emptySlots*size + mergeSpace, unknownSlots, stored };
}

// 读取主背包和快捷栏可接纳普通同类掉落的数量。
function inventoryCapacity(bot, item) {
  const { slots, inventoryStart, inventoryEnd } = bot.inventory ?? {};
  const { stored, ...capacity } = slotRange(slots,inventoryStart,inventoryEnd,item,bot.registry.itemsByName[item]?.stackSize);
  return { item, scope: 'main_and_hotbar', sampledAtMs: Math.floor(performance.now()), ...capacity };
}

// 按物品汇总主背包与快捷栏中已经同步的数量。
function inventory(bot) {
  const stock = {};
  for (const stack of bot.inventory.items()) stock[stack.name] = (stock[stack.name] ?? 0) + stack.count;
  return stock;
}

// 按同一物品名称比对两次实际库存，保留期间发生的所有净变化。
function inventoryDelta(before, after) {
  const delta = {};
  for (const name of new Set([...Object.keys(before), ...Object.keys(after)])) {
    const amount = (after[name] ?? 0) - (before[name] ?? 0);
    if (amount) delta[name] = amount;
  }
  return delta;
}

module.exports = { slotRange, inventoryCapacity, inventory, inventoryDelta };
