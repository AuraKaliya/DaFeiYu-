const { playerFocus } = require('./player-focus.cjs');

const hazards = new Set(['water', 'lava', 'bubble_column', 'fire', 'soul_fire', 'cactus', 'magma_block', 'powder_snow', 'sweet_berry_bush']);
const neighbors = [[1, 0], [-1, 0], [0, 1], [0, -1]];

// 以同一游戏高度区分实体障碍、可站立位置和未加载格。
function surface(bot, point) {
  const standing = [point, point.offset(0, 1, 0), point.offset(0, -1, 0)].map(pos => bot.blockAt(pos));
  if (standing.some(block => !block)) return '?';
  if (standing.some(block => hazards.has(block.name) || block.getProperties?.().waterlogged)) return 'x';
  const [feet, head, ground] = standing;
  if (feet.boundingBox !== 'empty' || head.boundingBox !== 'empty') return '#';
  return ground.boundingBox === 'block' ? '.' : 'x';
}

// 从原话附近找有连续落脚面的实际水平层，玩家站在墙顶时也能读其周围地面。
// 步骤：采样各候选高度 → 比较落脚覆盖 → 以离原位置最近的一层消除并列。
function groundLevel(bot, center) {
  let level = center.y, largest = 0;
  for (const offset of [0, -1, 1, -2, 2, -3, -4, -5]) {
    let floor = 0;
    for (let z = -4; z <= 4; ++z) for (let x = -4; x <= 4; ++x)
      if (surface(bot, center.offset(x, offset, z)) === '.') ++floor;
    if (floor > largest) { largest = floor; level = center.y + offset; }
  }
  return largest ? level : null;
}

// 收集同层相连的障碍格并标记边缘覆盖，未加载区不能证明墙已到尽头。
// 步骤：从可读实体格扩展 → 合并平面范围 → 标记截断和邻区未知。
function obstacle(rows, x, z, seen, origin) {
  const pending = [[x, z]];
  seen.add(`${x},${z}`);
  let minX = x, maxX = x, minZ = z, maxZ = z, complete = true;
  for (const [a, b] of pending) {
    minX = Math.min(minX, a); maxX = Math.max(maxX, a);
    minZ = Math.min(minZ, b); maxZ = Math.max(maxZ, b);
    for (const [dx, dz] of neighbors) {
      const i = a + dx, j = b + dz, key = `${i},${j}`;
      const token = rows[j]?.[i];
      if (token === '?' || token === undefined) complete = false;
      if (token !== '#' || seen.has(key)) continue;
      seen.add(key);
      pending.push([i, j]);
    }
  }
  const bounds = { minX: origin.x + minX, maxX: origin.x + maxX, minZ: origin.z + minZ, maxZ: origin.z + maxZ, y: origin.y };
  return { id: `${bounds.minX},${bounds.y},${bounds.minZ}:${bounds.maxX},${bounds.maxZ}`, bounds, complete,
    cells: pending.length, footprint: new Set(pending.map(([a, b]) => `${origin.x + a},${origin.z + b}`)) };
}

// 读取原话附近墙体、端点与两侧的落脚区域，视线命中与候选身份分别返回。
// 步骤：确定地面与覆盖 → 提取连通障碍 → 按原话视线筛选并保留歧义。
function observeWall(bot, anchor, position, radius = 8) {
  const center = position.floored();
  if (center.distanceTo(bot.entity.position) > 24) return { status: 'out_of_range' };
  const y = groundLevel(bot, center);
  if (y === null) return { status: 'coverage_required', reason: 'stand_level_unknown' };
  const origin = center.clone().set(center.x - radius, y, center.z - radius);
  const width = 2 * radius + 1;
  const rows = Array.from({ length: width }, (_, z) => Array.from({ length: width },
    (_, x) => surface(bot, origin.offset(x, 0, z))).join(''));
  const { hit } = playerFocus(bot, anchor, center, 'hit');
  const candidates = [], seen = new Set();
  const yaw = anchor.facing?.yaw;
  for (let z = 0; z < width; ++z) for (let x = 0; x < width; ++x) {
    if (rows[z][x] !== '#' || seen.has(`${x},${z}`)) continue;
    const { footprint, ...candidate } = obstacle(rows, x, z, seen, origin);
    if (candidate.cells < 2) continue;
    const b = candidate.bounds;
    const dx = Math.max(b.minX, Math.min(center.x, b.maxX)) - center.x;
    const dz = Math.max(b.minZ, Math.min(center.z, b.maxZ)) - center.z;
    const distance = Math.hypot(dx, dz);
    const pointed = Boolean(hit && hit.y >= y && footprint.has(`${hit.x},${hit.z}`));
    if (!pointed && (distance > 8 || (distance > 2 && yaw != null && -Math.sin(yaw) * dx - Math.cos(yaw) * dz < 0))) continue;
    candidates.push({ ...candidate, pointed, distance, axis: b.maxX - b.minX >= b.maxZ - b.minZ ? 'x' : 'z' });
  }
  candidates.sort((a, b) => Number(b.pointed) - Number(a.pointed) || a.distance - b.distance);
  const chosen = candidates.find(candidate => candidate.pointed) ?? (candidates.length === 1 ? candidates[0] : null);
  const status = chosen ? chosen.complete ? 'selected' : 'coverage_required' : candidates.length ? 'ambiguous' : 'none';
  return { status, choice: chosen?.id ?? null, candidates: candidates.slice(0, 8), hit,
    coverage: { origin, rows, unknown: rows.join('').replace(/[^?]/g, '').length } };
}

module.exports = { observeWall, surface };
