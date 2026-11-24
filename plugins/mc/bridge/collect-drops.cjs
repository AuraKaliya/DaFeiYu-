const { isDrop, dropStack } = require('./dropped-items.cjs');

// 追踪一次开挖附近新出现的一堆物品，按协议拾取事件记录数量和拾取实体。
// 步骤：排除既有实体并绑定本次目标 → 追踪同一实体和拾取包 → 结束时保留末次状态并释放监听。
function trackBreakDrop(bot, point, wanted, result, now = () => performance.now()) {
  const actor = bot.entity, dimension = bot.game.dimension;
  const known = new Set(Object.values(bot.entities));
  const center = point.offset(0.5, 0.5, 0.5);
  const evidence = result.pickups ??= { scope: 'one_new_stack_near_each_target', attempted: 0, omitted: 0,
    recent: [], selfCollected: {}, otherCollected: {} };
  const entry = { targetCell: { x: point.x, y: point.y, z: point.z }, state: 'unseen' };
  ++evidence.attempted;
  evidence.recent.push(entry);
  if (evidence.recent.length > 4) { evidence.recent.shift(); ++evidence.omitted; }
  let tracked;

  // 仅在原维度选择材料符合且初见位置接近本格的新实体。
  function consider(entity) {
    if (tracked || bot.entity !== actor || bot.game.dimension !== dimension || !isDrop(entity) ||
        known.has(entity) || bot.entities[entity.id] !== entity || !entity.position ||
        !(center.distanceTo(entity.position) <= 3)) return;
    const stack = dropStack(entity);
    if (!stack || (wanted && stack.item !== wanted)) return;
    tracked = entity;
    Object.assign(entry, { state: 'visible', entityId: entity.id, ...(entity.uuid && { uuid: entity.uuid }),
      item: stack.item, seenCount: stack.count, seenAtMs: Math.floor(now()), position: entity.position.clone(),
      selfCount: 0, otherCount: 0 });
  }
  // 保留实体离开本客户端视野的事实和时点。
  function gone(entity) {
    if (tracked && entity === tracked && entry.state === 'visible') {
      entry.state = 'left_view';
      entry.leftAtMs = Math.floor(now());
    }
  }
  // 在当前窗口读回同一堆物品，拒绝重用运行编号的新实体。
  function live() {
    if (!tracked) for (const entity of Object.values(bot.entities)) consider(entity);
    if (tracked && (bot.entity !== actor || bot.game.dimension !== dimension ||
        bot.entities[tracked.id] !== tracked || !isDrop(tracked))) gone(tracked);
    return entry.state === 'visible' ? tracked : undefined;
  }
  // 仅按游戏服务端发来的实体拾取包记账，数量与背包读回各自保存。
  function collected(packet) {
    const entity = bot.entities[packet.collectedEntityId];
    if (!tracked) consider(entity);
    if (!tracked || tracked !== entity || entry.state !== 'visible' || bot.entity !== actor ||
        bot.game.dimension !== dimension || !Number.isSafeInteger(packet.collectorEntityId) || packet.collectorEntityId < 0 ||
        !Number.isSafeInteger(packet.pickupItemCount) || packet.pickupItemCount <= 0) return;
    const self = packet.collectorEntityId === actor.id;
    const totals = self ? evidence.selfCollected : evidence.otherCollected;
    totals[entry.item] = (totals[entry.item] ?? 0) + packet.pickupItemCount;
    entry[self ? 'selfCount' : 'otherCount'] += packet.pickupItemCount;
    const uuid = self ? (actor.uuid ?? bot.player?.uuid) : bot.entities[packet.collectorEntityId]?.uuid;
    entry.lastCollector = { entityId: packet.collectorEntityId, ...(uuid && { uuid }),
      self, count: packet.pickupItemCount, sampledAtMs: Math.floor(now()) };
  }
  bot.on('itemDrop', consider);
  bot.on('entityGone', gone);
  bot._client.on('collect', collected);
  return { entry, live,
    // 在末次读回后同步移除本次开挖的三个监听。
    stop() {
      live();
      bot.removeListener('itemDrop', consider);
      bot.removeListener('entityGone', gone);
      bot._client.removeListener('collect', collected);
    },
  };
}

module.exports = { trackBreakDrop };
