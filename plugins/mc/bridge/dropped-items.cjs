const { inventoryCapacity } = require('./inventory-capacity.cjs');

// 识别当前游戏版本在客户端登记的掉落实体。
function isDrop(entity) {
  return entity?.isValid !== false && ['item', 'Item', 'item_stack'].includes(entity?.name);
}

// 读取实体当前携带的物品栈，元数据尚未同步时保留未知。
function dropStack(entity) {
  try {
    const stack = entity.getDroppedItem?.();
    if (typeof stack?.name === 'string' && Number.isSafeInteger(stack.count) && stack.count > 0)
      return { item: stack.name, count: stack.count };
  } catch { /* 实体生成与物品元数据可能分包到达。 */ }
  return null;
}

// 以角色实际位置为中心采样客户端追踪的掉落，保留过滤、截断和缺读范围。
// 步骤：绑定观察来源 → 按半径与物品筛选 → 返回最近的实体和覆盖计数。
function observeDrops(bot, options, id, generation) {
  const { radius, item } = options;
  if (item && !Object.hasOwn(bot.registry.itemsByName, item))
    throw Object.assign(new Error('当前游戏版本没有该物品'), { code: 'observe_drop_item_unknown' });
  const position = bot.entity?.position?.clone();
  if (!position || ![position.x, position.y, position.z].every(Number.isFinite))
    throw Object.assign(new Error('无法取得观察掉落所需的角色位置'), { code: 'observe_drop_position_unavailable' });
  const sample = { id, generation, dimension: bot.game.dimension, sampledAtMs: Math.floor(performance.now()),
    coverage: 'client_tracked_entities', position, radius, ...(item && { item }), unknownStack: 0, unlocated: 0 };
  const matches = [];
  for (const entity of Object.values(bot.entities)) {
    if (!isDrop(entity)) continue;
    const distance = entity.position ? position.distanceTo(entity.position) : NaN;
    if (!Number.isFinite(distance)) { ++sample.unlocated; continue; }
    if (distance > radius) continue;
    const stack = dropStack(entity);
    if (!stack) ++sample.unknownStack;
    if (item && stack?.item !== item) continue;
    matches.push({ entityId: entity.id, ...(entity.uuid && { uuid: entity.uuid }), position: entity.position.clone(),
      distance, item: stack?.item ?? null, count: stack?.count ?? null });
  }
  matches.sort((a, b) => a.distance - b.distance || a.entityId - b.entityId);
  return { ...sample, ...(item && { capacity: inventoryCapacity(bot, item) }),
    omitted: Math.max(0, matches.length - 12), entities: matches.slice(0, 12) };
}

module.exports = { isDrop, dropStack, observeDrops };
