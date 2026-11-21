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

// 读取两侧同材质的有界连续空格，未加载的格子不能成为开口。
// 步骤：核对起点和左边界 → 计算净空宽度 → 核对右边界。
function wallRow(bot, cell, dx, dz) {
  const left = bot.blockAt(cell.offset(-dx, 0, -dz));
  if (!solid(left) || !air(bot.blockAt(cell))) return null;
  let width = 1;
  while (width < 4 && air(bot.blockAt(cell.offset(width * dx, 0, width * dz)))) ++width;
  const right = bot.blockAt(cell.offset(width * dx, 0, width * dz));
  return solid(right) && right.name === left.name ? { block: left.name, width } : null;
}

// 查找有支撑且边界连续的墙体开口。
// 步骤：扫描原话邻域 → 复核连续净空和支撑 → 合并上层并返回稳定候选编号。
function wallGaps(bot, center) {
  const origin = center.floored();
  const gaps = [];
  for (let y = -3; y <= 2; ++y) for (let z = -5; z <= 5; ++z) for (let x = -5; x <= 5; ++x) {
    const cell = origin.offset(x, y, z);
    if (!air(bot.blockAt(cell)) || !solid(bot.blockAt(cell.offset(0, -1, 0)))) continue;
    for (const [dx, dz] of axes) {
      const row = wallRow(bot, cell, dx, dz);
      if (!row) continue;
      const { block, width } = row;
      if (x + (width - 1) * dx > 5 || z + (width - 1) * dz > 5) continue;
      let supported = true;
      for (let offset = 1; offset < width; ++offset)
        supported &&= solid(bot.blockAt(cell.offset(offset * dx, -1, offset * dz)));
      if (!supported) continue;
      const above = y < 2 && wallRow(bot, cell.offset(0, 1, 0), dx, dz);
      const height = above?.width === width && above.block === block ? 2 : 1;
      const axis = dx ? 'x' : 'z';
      gaps.push({ id: `${cell.x},${cell.y},${cell.z}:${axis}`, x: cell.x, y: cell.y, z: cell.z, width, height, axis, block });
    }
  }
  return gaps;
}

// 检查原话视线是否穿过开口中的任一空格。
// 步骤：遍历两层净空 → 按轴换算格点 → 命中任一格即返回。
function rayCrossesGap(crossed, gap) {
  for (let dy = 0; dy < gap.height; ++dy) for (let offset = 0; offset < gap.width; ++offset) {
    const x = gap.x + (gap.axis === 'x' ? offset : 0), z = gap.z + (gap.axis === 'z' ? offset : 0);
    if (crossed.has(`${x},${gap.y + dy},${z}`)) return true;
  }
  return false;
}

// 将原话视线和当前已加载候选对照，只有唯一穿过的格子组才形成指向。
// 步骤：采样视线与候选 → 按命中与原位置排序 → 交付指向或澄清状态。
function playerFocus(bot, anchor, center, focus = 'openings') {
  const { crossed, hit, unavailable } = sightLine(bot, anchor);
  if (focus === 'hit') return { status: unavailable ? 'unavailable' : hit ? 'hit' : 'none', hit: hit ?? null };
  const candidates = wallGaps(bot, center);
  for (const gap of candidates) gap.pointed = rayCrossesGap(crossed, gap);
  const distance = gap => Math.hypot(gap.x + (gap.axis === 'x' ? gap.width : 1) / 2 - center.x,
    gap.y - center.y, gap.z + (gap.axis === 'z' ? gap.width : 1) / 2 - center.z);
  candidates.sort((a, b) => Number(b.pointed) - Number(a.pointed) || distance(a) - distance(b));
  const pointed = candidates.filter(gap => gap.pointed);
  const choice = !unavailable && pointed.length === 1 ? pointed[0].id : null;
  return { status: choice ? 'pointed' : unavailable ? 'unavailable' : candidates.length ? 'ambiguous' : 'none',
    choice, hit: hit ?? null, candidates: candidates.slice(0, 8) };
}

module.exports = { playerFocus };
