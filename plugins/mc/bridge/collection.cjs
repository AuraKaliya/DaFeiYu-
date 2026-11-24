const { setTimeout: delay } = require('node:timers/promises');
const terrain = new Set(['grass_block', 'dirt', 'coarse_dirt', 'rooted_dirt', 'podzol', 'mycelium']);
const vacant = new Set(['air', 'cave_air', 'void_air']);
const neighbours = [[-1,0],[1,0],[0,-1],[0,1]];

// 从当前版本的默认掉落中选取允许的表层地形，动作仍须核对实际入包。
function groundTypes(registry, item) {
  const candidates = [...terrain].map(name => registry.blocksByName[name]);
  return candidates.filter(block => block?.drops.includes(registry.itemsByName[item]?.id));
}

// 判断方格是否属于地面取材范围，未加载的地形返回未知。
// 步骤：排除保护格 → 核对上下与邻接地形 → 区分已确认范围和缺失证据。
function groundScope(bot, block, protectedCells = []) {
  if (!block || !terrain.has(block.name)) return false;
  const pos = block.position;
  if (protectedCells.some(({ x, y, z }) => pos.x === x && pos.z === z && (pos.y === y || pos.y === y - 1)))
    return false;
  const above = bot.blockAt(pos.offset(0, 1, 0));
  const below = bot.blockAt(pos.offset(0, -1, 0));
  if ((above && !vacant.has(above.name)) || (below && below.boundingBox !== 'block')) return false;
  let known = Boolean(above && below);
  let level = 0;
  for (const [x, z] of neighbours) {
    const side = bot.blockAt(pos.offset(x, 0, z));
    const top = bot.blockAt(pos.offset(x, 1, z));
    if ((side && !terrain.has(side.name) && !vacant.has(side.name)) || (top && !vacant.has(top.name))) return false;
    if (!side || !top) known = false;
    if (terrain.has(side?.name)) ++level;
  }
  return known ? level >= 3 : null;
}

// 只在已确认的地面取材，并保留所有角色脚下的支撑。
function fromGround(bot, block, protectedCells = []) {
  if (groundScope(bot, block, protectedCells) !== true) return false;
  const pos = block.position;
  return ![bot.entity, ...Object.values(bot.players).map(player => player.entity)].some(entity => {
    const feet = entity?.position?.floored();
    return feet && feet.x === pos.x && feet.z === pos.z && feet.y >= pos.y && feet.y <= pos.y + 2;
  });
}

// 核对本次破坏与原确认候选的实际影响范围是否重叠。
// 步骤：按游戏注册表展开物品来源 → 对照每个受影响方格 → 核对原取材约束。
function requiresCollectionApproval(bot, guards, affected) {
  return guards?.some(guard => {
    const material = guard.block?.replace(/^minecraft:/, '');
    const item = guard.item?.replace(/^minecraft:/, '');
    const types = item && groundTypes(bot.registry, item);
    return affected.some(block => (!material || block.name === material) &&
      (!types || types.some(type => type.id === block.type)) &&
      (!Object.hasOwn(guard, 'x') || ['x', 'y', 'z'].every(axis => guard[axis] === block.position[axis])) &&
      (guard.source !== 'ground' || groundScope(bot, block, guard.protectedCells) !== false));
  }) ?? false;
}

// 查找这次破坏后出现且仍在附近的实际掉落物。
function nearbyDrop(bot, point, known) {
  let closest, distance = 3;
  for (const [id, entity] of Object.entries(bot.entities)) {
    if (known.has(id) || !entity.position || !['item', 'Item', 'item_stack'].includes(entity.name)) continue;
    const current = entity.position.distanceTo(point);
    if (current < distance) { distance = current; closest = entity; }
  }
  return closest;
}

// 核对原话参照仍紧邻目标下方，未知格与材料变更都不能通过。
function matchesDigReference(bot, point, reference) {
  if (!reference) return true;
  const below = point.offset(0, -1, 0);
  return below.x === reference.x && below.y === reference.y && below.z === reference.z &&
    bot.blockAt(below)?.name === reference.block;
}

// 沿实际掉落物所在列寻找已加载且有支撑的拾取站位。
function pickupStand(bot, point) {
  for (let y = 0; y >= -4; --y) {
    const feet = point.offset(0, y, 0);
    const space = bot.blockAt(feet);
    if (!space || space.boundingBox === 'block') break;
    if (vacant.has(space.name) && vacant.has(bot.blockAt(feet.offset(0, 1, 0))?.name) &&
        bot.blockAt(feet.offset(0, -1, 0))?.boundingBox === 'block') return feet;
  }
  return null;
}

// 展开这次破坏会直接影响的格，门的上下两格共用一个动作。
function affectedByDig(bot, block) {
  const cells = [block];
  if (block.name.endsWith('_door')) {
    const half = block.getProperties().half;
    const other = bot.blockAt(block.position.offset(0, half === 'upper' ? -1 : 1, 0));
    if (other?.name === block.name && other.getProperties().half !== half) cells.push(other);
  }
  return cells;
}

// 记录实际变化格，并对同一批已核验的目标逐格读回。
function collectReadback(bot, block, cells = affectedByDig(bot, block)) {
  return async changes => {
    const pending = new Set(cells);
    for (let attempt = 0; attempt < 5; ++attempt) {
      for (const cell of pending) {
        const { position, name } = cell;
        const after = bot.blockAt(position);
        if (!after || after.name === name) continue;
        const { x, y, z } = position;
        changes.push({ x, y, z, before: name, after: after.name });
        pending.delete(cell);
      }
      if (!pending.size || attempt === 4) break;
      await delay(50);
    }
  };
}

// 按原工作轮询采样采掘阶段、已读回的破坏数和目标物品的当前库存。
// 步骤：记住本次目标与阶段起点 → 读取实效和库存 → 发布同一工作的新样本。
function collectionProgress(result, readStock, now = () => performance.now()) {
  let phase = 'selecting', phaseSinceMs = Math.floor(now()), target, lastBreakAtMs, broken = 0;
  // 采样时间随轮询更新，只有实际破坏数增长才更新破坏时点。
  function sample() {
    const sampledAtMs = Math.floor(now());
    if (result.broken > broken) { broken = result.broken; lastBreakAtMs = sampledAtMs; }
    const progress = result.progress = { phase, phaseSinceMs, sampledAtMs, broken, ...target };
    if (lastBreakAtMs !== undefined) progress.lastBreakAtMs = lastBreakAtMs;
    if (result.stock) progress.stock = { item: result.stock.item, required: result.stock.required, current: readStock() };
  }
  sample();
  return { sample,
    // 换目标时清除上一格，后续阶段沿用已确认的本次目标格和材料。
    enter(next, block) {
      phase = next;
      phaseSinceMs = Math.floor(now());
      if (phase === 'selecting') target = undefined;
      if (block) {
        const { x, y, z } = block.position;
        target = { targetCell: { x, y, z }, block: block.name };
      }
      sample();
    },
  };
}

module.exports = { fromGround, groundTypes, requiresCollectionApproval, affectedByDig, collectReadback, collectionProgress,
  pickupStand, nearbyDrop, matchesDigReference };
