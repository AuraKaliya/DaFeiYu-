const { isDrop, dropStack } = require('./dropped-items.cjs');

// 在当前角色与维度内锁定一堆符合条件的物品，记录实际拾取包和离开视野的时点。
// 步骤：绑定同一实体对象 → 对照游戏包记下收取人和数量 → 读回末次状态并释放监听。
function trackDrop(bot, entry, select, onPickup, now = () => performance.now()) {
  const actor = bot.entity, dimension = bot.game.dimension;
  let tracked;

  // 实体、物品元数据和来源条件都可核验时才开始追踪。
  function consider(entity) {
    if (tracked || bot.entity !== actor || bot.game.dimension !== dimension || !isDrop(entity) ||
        bot.entities[entity.id] !== entity || !entity.position) return;
    const stack = dropStack(entity);
    if (!stack || !select(entity, stack)) return;
    tracked = entity;
    Object.assign(entry, { state: 'visible', entityId: entity.id, ...(entity.uuid && { uuid: entity.uuid }),
      item: stack.item, seenCount: stack.count, seenAtMs: Math.floor(now()), position: entity.position.clone(),
      selfCount: 0, otherCount: 0 });
  }
  // 保存本客户端最后看到原堆的时点，失去视野后不再改绑运行编号。
  function gone(entity) {
    if (tracked && entity === tracked && entry.state === 'visible') {
      entry.state = 'left_view';
      entry.leftAtMs = Math.floor(now());
    }
  }
  // 轮询时补读尚未同步的候选，并复核原实体的现场身份。
  function live() {
    if (!tracked) for (const entity of Object.values(bot.entities)) consider(entity);
    if (tracked && (bot.entity !== actor || bot.game.dimension !== dimension ||
        bot.entities[tracked.id] !== tracked || !isDrop(tracked))) gone(tracked);
    return entry.state === 'visible' ? tracked : undefined;
  }
  // 只把属于原堆的服务端收取事件记入实效，库存另由工作读取。
  function collected(packet) {
    const entity = bot.entities[packet.collectedEntityId];
    if (!tracked) consider(entity);
    if (!tracked || tracked !== entity || entry.state !== 'visible' || bot.entity !== actor ||
        bot.game.dimension !== dimension || !Number.isSafeInteger(packet.collectorEntityId) || packet.collectorEntityId < 0 ||
        !Number.isSafeInteger(packet.pickupItemCount) || packet.pickupItemCount <= 0) return;
    const self = packet.collectorEntityId === actor.id;
    entry[self ? 'selfCount' : 'otherCount'] += packet.pickupItemCount;
    const uuid = self ? (actor.uuid ?? bot.player?.uuid) : bot.entities[packet.collectorEntityId]?.uuid;
    entry.lastCollector = { entityId: packet.collectorEntityId, ...(uuid && { uuid }),
      self, count: packet.pickupItemCount, sampledAtMs: Math.floor(now()) };
    onPickup?.(entry);
  }
  bot.on('itemDrop', consider);
  bot.on('entityGone', gone);
  bot._client.on('collect', collected);
  return { entry, live,
    // 读回最终可见状态并同步释放本工作的监听。
    stop() {
      try { live(); }
      finally {
        bot.removeListener('itemDrop', consider);
        bot.removeListener('entityGone', gone);
        bot._client.removeListener('collect', collected);
      }
    },
  };
}

module.exports = { trackDrop };
