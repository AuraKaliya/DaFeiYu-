// 监听服务端给当前角色的生命包及空气元数据，两类读数各自保留实际收包时间。
// 步骤：绑定当前出生和实体 → 分别记录本人读数 → 查询时核对同一现场 → 关闭后归还监听。
function trackSelfVitals(bot, currentGeneration) {
  let health, air, closed = false;
  // 把一次收包与当时的角色对象、出生代次和维度固定在一起。
  function capture(reading) {
    const generation = currentGeneration();
    if (!generation || !Number.isInteger(bot.entity?.id) || !bot.player?.uuid || !bot.game?.dimension) return;
    return { generation, entity: bot.entity, entityId: bot.entity.id, actor: bot.player.uuid, dimension: bot.game.dimension,
      reading: { state: 'sampled', sampledAtMs: Math.floor(performance.now()), ...reading } };
  }
  // 生命与饥饿来自本连接的同一份 update_health 包。
  function onHealth(packet) {
    health = Number.isFinite(packet.health) && packet.health >= 0 && Number.isInteger(packet.food) && packet.food >= 0 && packet.food <= 20
      ? capture({ source: 'update_health', points: packet.health, food: packet.food }) : undefined;
  }
  // 只认本人运行时实体号和当前版本注册表命名的空气字段，保留有符号游戏刻数。
  function onMetadata(packet) {
    if (!bot.entity || packet.entityId !== bot.entity.id) return;
    const index = bot.registry?.entitiesByName?.player?.metadataKeys?.indexOf('air_supply') ?? -1;
    if (index < 0) return;
    const field = packet.metadata.find(row => row.key === index);
    if (field) air = Number.isInteger(field.value) && field.value >= -2147483648 && field.value <= 2147483647
      ? capture({ source: 'entity_metadata.air_supply', ticks: field.value }) : undefined;
  }
  bot._client?.on('update_health',onHealth);
  bot._client?.on('entity_metadata',onMetadata);

  return {
    // 仅复制与本次原工作和当前角色相符的读数，重复查询保留原来的采样时刻。
    read(id, generation) {
      const entity = bot.entity, actor = bot.player?.uuid, dimension = bot.game?.dimension;
      if (closed || !generation || currentGeneration() !== generation || !actor || !dimension ||
          !Number.isInteger(entity?.id) || entity.id < 0 || entity.id > 2147483647)
        throw Object.assign(new Error('当前出生或角色不可用于读取生命状态'), { code: 'observe_vitals_unavailable' });
      const same = entry => entry && entry.generation === generation && entry.entity === entity && entry.entityId === entity.id &&
        entry.actor === actor && entry.dimension === dimension
        ? { ...entry.reading } : { state: 'unknown' };
      return { id, generation, actor, entityId: entity.id, dimension, readAtMs: Math.floor(performance.now()),
        health: same(health), air: same(air) };
    },
    // 结束连接时释放两个协议监听及关联的实体。
    close() {
      closed = true;
      bot._client?.removeListener('update_health',onHealth);
      bot._client?.removeListener('entity_metadata',onMetadata);
      health = air = undefined;
    },
  };
}

module.exports = { trackSelfVitals };
