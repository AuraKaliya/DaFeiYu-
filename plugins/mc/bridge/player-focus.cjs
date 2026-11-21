// 扫描区分空气与不可读的格子。
const air = block => block && ['air', 'cave_air', 'void_air'].includes(block.name);
// 墙体侧面和支撑都需要实心方块。
const solid = block => block?.boundingBox === 'block';
const axes = [[1, 0], [0, 1]];

// 由原话眼位与朝向读取近处经过的格子，遇到未加载格或遮挡即停止。
// 步骤：重建视线 → 逐格读已加载世界 → 记录命中或缺失。
function sightLine(bot, anchor) {
  const crossed = new Set();
  if (!anchor.eye || !anchor.facing) return { crossed, unavailable: true };
  const eye = bot.entity.position.clone().set(anchor.eye.x, anchor.eye.y, anchor.eye.z);
  const { yaw, pitch } = anchor.facing;
  const dx = -Math.sin(yaw) * Math.cos(pitch), dy = Math.sin(pitch), dz = -Math.cos(yaw) * Math.cos(pitch);
  for (let distance = 0; distance <= 6; distance += 0.125) {
    const { x, y, z } = eye.offset(dx * distance, dy * distance, dz * distance).floored();
    const key = `${x},${y},${z}`;
    if (crossed.has(key)) continue;
    const block = bot.blockAt(eye.clone().set(x, y, z));
    if (!block) return { crossed, unavailable: true };
    if (!air(block)) return { crossed, hit: { x, y, z, block: block.name } };
    crossed.add(key);
  }
  return { crossed };
}

// 查找有支撑、两侧材料相同的一格或两格墙体开口。
// 步骤：扫描原话邻域 → 复核相邻实心格和净空 → 返回稳定候选编号。
function wallGaps(bot, center) {
  const origin = center.floored();
  const material = (cell, dx, dz) => {
    const left = bot.blockAt(cell.offset(-dx, 0, -dz)), right = bot.blockAt(cell.offset(dx, 0, dz));
    return solid(left) && solid(right) && left.name === right.name ? left.name : null;
  };
  const gaps = [];
  for (let y = -3; y <= 2; ++y) for (let z = -5; z <= 5; ++z) for (let x = -5; x <= 5; ++x) {
    const cell = origin.offset(x, y, z);
    if (!air(bot.blockAt(cell)) || !solid(bot.blockAt(cell.offset(0, -1, 0)))) continue;
    for (const [dx, dz] of axes) {
      const block = material(cell, dx, dz);
      if (!block) continue;
      const above = cell.offset(0, 1, 0);
      const height = y < 2 && air(bot.blockAt(above)) && material(above, dx, dz) === block ? 2 : 1;
      const axis = dx ? 'x' : 'z';
      gaps.push({ id: `${cell.x},${cell.y},${cell.z}:${axis}`, x: cell.x, y: cell.y, z: cell.z, height, axis, block });
    }
  }
  return gaps;
}

// 将原话视线和当前已加载候选对照，只有唯一穿过的格子组才形成指向。
// 步骤：采样视线与候选 → 按命中与原位置排序 → 交付指向或澄清状态。
function playerFocus(bot, anchor, center) {
  const { crossed, hit, unavailable } = sightLine(bot, anchor);
  const candidates = wallGaps(bot, center);
  for (const gap of candidates) {
    gap.pointed = crossed.has(`${gap.x},${gap.y},${gap.z}`) ||
      (gap.height === 2 && crossed.has(`${gap.x},${gap.y + 1},${gap.z}`));
  }
  const distance = gap => Math.hypot(gap.x + 0.5 - center.x, gap.y - center.y, gap.z + 0.5 - center.z);
  candidates.sort((a, b) => Number(b.pointed) - Number(a.pointed) || distance(a) - distance(b));
  const pointed = candidates.filter(gap => gap.pointed);
  const choice = !unavailable && pointed.length === 1 ? pointed[0].id : null;
  return { status: choice ? 'pointed' : unavailable ? 'unavailable' : candidates.length ? 'ambiguous' : 'none',
    choice, hit: hit ?? null, candidates: candidates.slice(0, 8) };
}

module.exports = { playerFocus };
