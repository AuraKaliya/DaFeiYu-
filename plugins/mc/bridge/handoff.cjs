const uuid = /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/;

// 校验交付对象、物品和指定总数，查询与实际操作复用同一参数范围。
function handoffInput(args) {
  if (typeof args.recipient !== 'string' || !uuid.test(args.recipient) || typeof args.item !== 'string' ||
      !/^(?:minecraft:)?[a-z0-9_]{1,80}$/.test(args.item) || !Number.isSafeInteger(args.count) || args.count < 1 || args.count > 2304)
    throw Object.assign(new Error('交物需要配对玩家 UUID、物品和 1～2304 的总数'), { code: 'handoff_parameters' });
  return { recipient: args.recipient, item: args.item.replace(/^minecraft:/, ''), count: args.count };
}

// 从主背包和快捷栏选择足够的普通物品，分别记录已读余量、特殊栈和缺读槽位。
// 步骤：核实窗口范围 → 逐槽选取指定总数 → 保留不足数量与覆盖情况。
function handoffStock(bot, item, count) {
  const { slots, inventoryStart: start, inventoryEnd: end } = bot.inventory ?? {};
  const size = bot.registry.itemsByName[item]?.stackSize;
  const stock = { scope: 'main_and_hotbar', available: 0, selected: [], missing: count, excluded: 0, unknownSlots: 0 };
  if (!Number.isInteger(size) || size < 1 || !Array.isArray(slots) || !Number.isInteger(start) || !Number.isInteger(end) ||
      start < 0 || end <= start || end > slots.length || end-start > 128) return { ...stock, status: 'unavailable' };
  for (let slot = start; slot < end; ++slot) {
    const entry = slots[slot];
    if (entry === null) continue;
    if (!entry?.name || !Number.isInteger(entry.count) || entry.count < 1) { ++stock.unknownSlots; continue; }
    if (entry.name !== item) continue;
    if (entry.count > size || entry.nbt || entry.metadata || entry.components?.length || entry.removedComponents?.length) {
      ++stock.excluded;
      continue;
    }
    stock.available += entry.count;
    if (stock.missing) {
      const take = Math.min(stock.missing, entry.count);
      stock.selected.push({ slot, held: entry.count, take });
      stock.missing -= take;
    }
  }
  return { ...stock, start, end, stackSize: size, status: stock.unknownSlots ? 'partial' : 'sampled' };
}

// 绑定同一配对玩家的当前实体，并用角色实际可见的位置和窗口核对交物条件。
// 步骤：核对接收者与游戏注册表 → 分别读取双方位置、距离与可见性 → 选取普通库存。
function observeHandoff(bot, owner, input, id, generation) {
  const { recipient, item, count } = handoffInput(input);
  if (recipient !== owner) throw Object.assign(new Error('本次仅能向当前配对玩家交物'), { code: 'handoff_recipient_unbound' });
  if (!Object.hasOwn(bot.registry.itemsByName, item))
    throw Object.assign(new Error('当前游戏版本没有该物品'), { code: 'handoff_item_unknown' });
  const player = Object.values(bot.players).find(p => p.uuid === recipient);
  const entity = player?.entity, actor = bot.entity;
  const position = e => e?.position && [e.position.x,e.position.y,e.position.z].every(Number.isFinite) ? e.position.clone() : null;
  const self = position(actor);
  const target = { state: player ? 'not_tracked' : 'not_listed' };
  if (entity && entity.uuid === recipient && entity.isValid !== false && bot.entities[entity.id] === entity) {
    const point = position(entity);
    Object.assign(target, { state: point ? 'visible' : 'unlocated', entityId: entity.id });
    if (point) target.position = point;
    if (self && point) target.distance = self.distanceTo(point);
  }
  return { id, generation, dimension: bot.game.dimension, sampledAtMs: Math.floor(performance.now()), recipient, item, count,
    actor: { uuid: actor?.uuid ?? bot.player?.uuid ?? null, position: self }, target,
    window: bot.currentWindow ? 'container' : bot.inventory?.selectedItem ? 'cursor_occupied' : 'inventory',
    stock: handoffStock(bot,item,count) };
}

module.exports = { handoffInput, observeHandoff };
