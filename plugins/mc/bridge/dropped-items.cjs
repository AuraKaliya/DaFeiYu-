const { inventoryCapacity } = require('./inventory-capacity.cjs');

// 验证由原掉落观察编号和游戏实体 UUID 构成的引用。
function validDropReference(args) {
  return typeof args.observation === 'string' && args.observation.length > 0 && args.observation.length <= 128 &&
    typeof args.entity === 'string' && /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/.test(args.entity);
}

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

// 复查原观察中的同一 UUID，并记录当前距离、物品栈和原观察半径的覆盖情况。
// 步骤：核对候选来源 → 按稳定身份查找当前实体 → 保留不可见、位置缺失或物品栈缺读。
function readDropTarget(bot, source, uuid) {
  const original = source.entities.find(entry => entry.uuid === uuid);
  if (!original) throw Object.assign(new Error('原观察没有返回这个掉落实体'), { code: 'drop_not_in_observation' });
  const sample = { observation: source.id, entity: uuid, generation: source.generation, dimension: source.dimension,
    observedAtMs: source.sampledAtMs, sampledAtMs: Math.floor(performance.now()), radius: source.radius,
    original: { entityId: original.entityId, item: original.item, count: original.count } };
  const entity = Object.values(bot.entities).find(value => value.uuid === uuid && isDrop(value));
  if (!entity) return { ...sample, state: 'not_tracked' };
  sample.entityId = entity.id;
  const p = entity.position, center = source.position, self = bot.entity?.position;
  if (!p || !self || ![p.x,p.y,p.z,self.x,self.y,self.z].every(Number.isFinite))
    return { ...sample, state: 'unlocated' };
  sample.position = p.clone();
  sample.distance = p.distanceTo(self);
  sample.withinOriginalRadius = p.distanceTo(center) <= source.radius;
  const stack = dropStack(entity);
  return stack ? { ...sample, state: 'tracked', ...stack } : { ...sample, state: 'stack_unknown' };
}

module.exports = { isDrop, dropStack, observeDrops, validDropReference, readDropTarget };
