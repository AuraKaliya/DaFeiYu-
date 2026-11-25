// 从已加载方块识别水体，含水方块也保留其实际水属性。
function waterBlock(block) {
  return block?.name === 'water' || block?.name === 'bubble_column' || block?.getProperties?.().waterlogged === true;
}

// 归并同一次本人空气耗尽，两个连续递增的正读数证实开始恢复。
// 步骤：首个非正读数建档 → 累计同次采样和最低值 → 短时回落归入原档，恢复后重新耗尽另开号。
function advanceAirEpisode(prior, reading, nextId) {
  const { ticks, sampledAtMs } = reading;
  if (!prior || prior.phase === 'recovered') {
    if (ticks > 0) return prior;
    return { episode: nextId(), phase: 'depleted', samples: 1, firstAtMs: sampledAtMs, lastAtMs: sampledAtMs,
      firstTicks: ticks, minimumTicks: ticks, lastTicks: ticks };
  }
  const next = { ...prior, samples: prior.samples+1, lastAtMs: sampledAtMs, lastTicks: ticks, minimumTicks: Math.min(prior.minimumTicks,ticks) };
  if (ticks <= 0) {
    next.phase = 'depleted';
    delete next.recoveryTicks;
    delete next.recoveryAtMs;
  } else if (prior.phase === 'recovering' && ticks > prior.lastTicks) {
    next.phase = 'recovered';
  } else {
    next.phase = 'recovering';
    if (prior.phase !== 'recovering' || ticks < prior.lastTicks) {
      next.recoveryTicks = ticks;
      next.recoveryAtMs = sampledAtMs;
    }
  }
  return next;
}

// 监听服务端给当前角色的生命包及空气元数据，两类读数各自保留实际收包时间。
// 步骤：绑定当前出生和实体 → 分别记录本人读数 → 查询时核对同一现场 → 关闭后归还监听。
function trackSelfVitals(bot, currentGeneration) {
  let health, air, closed = false;
  let episode = 0, sequence = 0, lastNotice;
  // 核对已记录样本与正在读取的出生、实体对象和地图一致。
  function belongs(entry,generation) {
    return entry && entry.generation === generation && entry.entity === bot.entity && entry.entityId === bot.entity?.id &&
      entry.actor === bot.player?.uuid && entry.dimension === bot.game?.dimension;
  }
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
  // 只认本人运行时实体号和当前版本注册表命名的空气字段，按实际读数归并同次耗尽。
  // 步骤：核对实体和字段 → 绑定有效空气样本 → 更新同次记录并保留最新状态边界。
  function onMetadata(packet) {
    if (!bot.entity || packet.entityId !== bot.entity.id) return;
    const index = bot.registry?.entitiesByName?.player?.metadataKeys?.indexOf('air_supply') ?? -1;
    if (index < 0) return;
    const field = packet.metadata.find(row => row.key === index);
    if (!field) return;
    const current = Number.isInteger(field.value) && field.value >= -2147483648 && field.value <= 2147483647
      ? capture({ source: 'entity_metadata.air_supply', ticks: field.value }) : undefined;
    if (current) {
      const feet = bot.entity.position?.floored?.();
      if (feet && [feet.x,feet.y,feet.z].every(Number.isSafeInteger) && (bot.entity.isInWater || waterBlock(bot.blockAt?.(feet))))
        current.reading.waterCell = { x:feet.x, y:feet.y, z:feet.z };
      const prior = belongs(air,current.generation) ? air.episode : undefined;
      current.episode = advanceAirEpisode(prior,current.reading,() => ++episode);
      const next = current.episode;
      if (next && (prior?.episode !== next.episode || (next.phase === 'recovered' && prior.phase !== 'recovered')))
        lastNotice = { ...current, sequence: ++sequence };
    }
    air = current;
  }
  bot._client?.on('update_health',onHealth);
  bot._client?.on('entity_metadata',onMetadata);

  return {
    // 按独立游标重读最近一次耗尽或恢复边界，连续低值只在原事件中累计。
    notice(generation, after) {
      if (closed || !belongs(lastNotice,generation) || currentGeneration() !== generation || lastNotice.sequence <= after) return;
      const { sequence, actor, entityId, dimension, episode: airEpisode } = lastNotice;
      return { sequence, generation, actor, entityId, dimension, source: 'entity_metadata.air_supply', airEpisode: { ...airEpisode } };
    },
    // 仅复制与本次原工作和当前角色相符的读数，重复查询保留原来的采样时刻。
    // 步骤：核对出生、实体与地图 → 分别选取有来源的状态 → 附上同次空气耗尽记录。
    read(id, generation) {
      const entity = bot.entity, actor = bot.player?.uuid, dimension = bot.game?.dimension;
      if (closed || !generation || currentGeneration() !== generation || !actor || !dimension ||
          !Number.isInteger(entity?.id) || entity.id < 0 || entity.id > 2147483647)
        throw Object.assign(new Error('当前出生或角色不可用于读取生命状态'), { code: 'observe_vitals_unavailable' });
      const same = entry => belongs(entry,generation) ? { ...entry.reading } : { state: 'unknown' };
      return { id, generation, actor, entityId: entity.id, dimension, readAtMs: Math.floor(performance.now()),
        health: same(health), air: same(air), ...(belongs(air,generation) && air.episode && { airEpisode: { ...air.episode } }) };
    },
    // 结束连接时释放两个协议监听及关联的实体。
    close() {
      closed = true;
      bot._client?.removeListener('update_health',onHealth);
      bot._client?.removeListener('entity_metadata',onMetadata);
      health = air = lastNotice = undefined;
    },
  };
}

module.exports = { trackSelfVitals, waterBlock };
