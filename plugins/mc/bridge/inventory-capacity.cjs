// 读取主背包和快捷栏可接纳普通同类掉落的数量，特殊物品栈保留未确认容量。
// 步骤：核对版本堆叠上限和窗口范围 → 统计空格与可合堆余量 → 标明缺读范围。
function inventoryCapacity(bot, item) {
  const sample = { item, scope: 'main_and_hotbar', sampledAtMs: Math.floor(performance.now()) };
  const size = bot.registry.itemsByName[item]?.stackSize;
  const { slots, inventoryStart: start, inventoryEnd: end } = bot.inventory ?? {};
  if (!Number.isInteger(size) || size <= 0 || !Array.isArray(slots) || !Number.isInteger(start) ||
      !Number.isInteger(end) || start < 0 || end <= start || end > slots.length) return { ...sample, status: 'unavailable' };

  let emptySlots = 0, mergeSpace = 0, unknownSlots = 0;
  for (let index = start; index < end; ++index) {
    const stack = slots[index];
    if (stack === null) { ++emptySlots; continue; }
    if (!stack?.name) { ++unknownSlots; continue; }
    if (stack.name !== item) continue;
    if (stack.nbt || stack.metadata || stack.components?.length || stack.removedComponents?.length ||
        !Number.isInteger(stack.count) || stack.count <= 0 || stack.count > size) { ++unknownSlots; continue; }
    mergeSpace += size - stack.count;
  }
  return { ...sample, status: unknownSlots ? 'partial' : 'sampled', slots: end-start, stackSize: size,
    emptySlots, mergeSpace, knownSpace: emptySlots*size + mergeSpace, unknownSlots };
}

module.exports = { inventoryCapacity };
