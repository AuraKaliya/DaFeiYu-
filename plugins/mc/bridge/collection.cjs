const { setTimeout: delay } = require('node:timers/promises');
const terrain = new Set(['grass_block', 'dirt', 'coarse_dirt', 'rooted_dirt', 'podzol', 'mycelium']);
const vacant = new Set(['air', 'cave_air', 'void_air']);
const neighbours = [[-1,0],[1,0],[0,-1],[0,1]];

// 只接纳有承重地层、开阔表面且远离突起结构的自然取材格。
// 步骤：排除目标墙格及其支撑 → 核对地形和邻接结构 → 保留角色脚下支撑。
function fromGround(bot, block, protectedCells = []) {
  if (!block || !terrain.has(block.name)) return false;
  const pos = block.position;
  if (protectedCells.some(({ x, y, z }) => pos.x === x && pos.z === z && (pos.y === y || pos.y === y - 1)))
    return false;
  if (!vacant.has(bot.blockAt(pos.offset(0, 1, 0))?.name) ||
      bot.blockAt(pos.offset(0, -1, 0))?.boundingBox !== 'block') return false;
  let level = 0;
  for (const [x, z] of neighbours) {
    const side = bot.blockAt(pos.offset(x, 0, z));
    if (!side || !vacant.has(bot.blockAt(pos.offset(x, 1, z))?.name) ||
        (!terrain.has(side.name) && !vacant.has(side.name))) return false;
    if (terrain.has(side.name)) ++level;
  }
  if (level < 3) return false;
  return ![bot.entity, ...Object.values(bot.players).map(player => player.entity)].some(entity => {
    const feet = entity?.position?.floored();
    return feet && feet.x === pos.x && feet.z === pos.z && feet.y >= pos.y && feet.y <= pos.y + 2;
  });
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

// 记录实际变化格，门的一次采掘可能同时更改两个服务端方格。
// 步骤：展开可核对的门伴随格 → 捕获破坏前名称 → 在写入完成后逐格比较。
function collectReadback(bot, block) {
  const cells = [block];
  if (block.name.endsWith('_door')) {
    const half = block.getProperties().half;
    const other = bot.blockAt(block.position.offset(0, half === 'upper' ? -1 : 1, 0));
    if (other?.name === block.name && other.getProperties().half !== half) cells.push(other);
  }
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

module.exports = { fromGround, collectReadback, pickupStand, nearbyDrop };
