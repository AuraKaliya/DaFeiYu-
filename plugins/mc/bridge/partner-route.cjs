const { playerFocus } = require('./player-focus.cjs');

const hazards = new Set(['water', 'lava', 'bubble_column', 'fire', 'soul_fire', 'cactus', 'magma_block', 'powder_snow', 'sweet_berry_bush']);
const neighbors = [[1, 0], [-1, 0], [0, 1], [0, -1]];
const corners = [[-1, -1], [1, -1], [1, 1], [-1, 1]];

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

// 读取观测层的一格，覆盖之外的坐标始终保持未知。
function terrain(coverage, x, z) { return coverage.rows[z - coverage.origin.z]?.[x - coverage.origin.x] ?? '?'; }

// 只接受墙体外侧的角区，直行到另一面须绕过对应墙端。
function cornerOf({ minX, maxX, minZ, maxZ }, { x, z }) {
  const sx = x < minX ? -1 : x > maxX ? 1 : 0;
  const sz = z < minZ ? -1 : z > maxZ ? 1 : 0;
  return corners.findIndex(([a, b]) => a === sx && b === sz);
}

// 将一段路径限制在共有的一侧或一端，允许寻路在该区域内避开障碍。
function routeCorridor(bounds, from, to, point) {
  if (from.x < bounds.minX && to.x < bounds.minX) return point.x < bounds.minX;
  if (from.x > bounds.maxX && to.x > bounds.maxX) return point.x > bounds.maxX;
  if (from.z < bounds.minZ && to.z < bounds.minZ) return point.z < bounds.minZ;
  return from.z > bounds.maxZ && to.z > bounds.maxZ && point.z > bounds.maxZ;
}

// 从同一份覆盖中寻找四个不同角区的一组落脚点。
function outerPoints(bounds, coverage, distance) {
  return corners.map(([x, z]) => {
    const point = { x: x < 0 ? bounds.minX - distance : bounds.maxX + distance, y: bounds.y,
      z: z < 0 ? bounds.minZ - distance : bounds.maxZ + distance };
    return terrain(coverage, point.x, point.z) === '.' ? point : null;
  });
}

// 核对四个角区依序闭合，并用与实际走路同源的寻路器验收整圈。
// 步骤：绑定观察中的墙 → 核对落脚和角区顺序 → 逐段核验路径及覆盖。
function validateRoute(scan, wall, waypoints, findPath) {
  const reject = reasonCode => ({ status: 'blocked', reasonCode });
  if (scan.choice && wall && wall !== scan.choice) return reject('wall_conflicts_with_pointing');
  const chosen = scan.candidates?.find(candidate => candidate.id === (wall ?? scan.choice));
  if (!chosen) return reject(scan.status === 'ambiguous' ? 'wall_ambiguous' : 'wall_not_observed');
  if (!chosen.complete) return reject('wall_coverage_required');
  const { bounds } = chosen, { coverage } = scan;
  let result = reject('waypoint_not_standable');
  for (const points of waypoints ? [waypoints] : [1, 2, 3].map(distance => outerPoints(bounds, coverage, distance))) {
    if (points.length !== 4 || points.some(point => !point || ![point.x, point.y, point.z].every(Number.isSafeInteger)))
      continue;
    if (points.some(point => terrain(coverage, point.x, point.z) === '?')) {
      result = reject('route_coverage_required');
      continue;
    }
    if (points.some(point => point.y !== bounds.y || terrain(coverage, point.x, point.z) !== '.')) continue;
    const visit = points.map(point => cornerOf(bounds, point));
    const clockwise = (visit[1] - visit[0] + 4) % 4;
    if (visit.includes(-1) || new Set(visit).size !== 4 || ![1, 3].includes(clockwise) ||
      visit.some((value, i) => (visit[(i + 1) % 4] - value + 4) % 4 !== clockwise))
      return reject('checkpoints_do_not_surround_wall');

    const lengths = [];
    for (let i = 0; i < points.length; ++i) {
      const from = points[i], to = points[(i + 1) % 4];
      const allowed = point => routeCorridor(bounds, from, to, point) && terrain(coverage, point.x, point.z) === '.';
      const nodes = findPath(from, to, allowed);
      const end = nodes?.at(-1);
      if (!end || end.x !== to.x || end.y !== to.y || end.z !== to.z ||
        nodes.some(node => node.y !== bounds.y || !allowed(node))) break;
      lengths.push(nodes.length);
    }
    if (lengths.length === 4) return { status: 'validated', wall: chosen.id, bounds, waypoints: points, lengths };
    result = reject(coverage.unknown ? 'route_coverage_required' : 'route_not_passable');
  }
  return result;
}

// 按实际命中顺序记录检查点，只有走完四段并回到起点才能累加整圈。
// 步骤：引导到起点 → 沿四侧核验真实站位 → 发布段进度与总圈数。
async function runRoute(route, requested, result, movement) {
  const progress = result.route = { id: route.id, requested, laps: 0, checkpoints: [] };
  const { waypoints, bounds } = route;
  const count = requested * waypoints.length;
  for (let index = 0; index <= count; ++index) {
    const target = waypoints[index % waypoints.length];
    const previous = index ? waypoints[(index - 1) % waypoints.length] : null;
    const allowed = previous ? point => routeCorridor(bounds, previous, target, point) : undefined;
    movement.check();
    const position = await movement.move(target, allowed);
    movement.check();
    if (!position || Math.floor(position.x) !== target.x || Math.floor(position.y) !== target.y || Math.floor(position.z) !== target.z)
      throw Object.assign(new Error('未进入本段核验站位'), { code: 'route_checkpoint_unverified' });
    progress.checkpoints.push({ index, position });
    progress.laps = Math.floor(index / waypoints.length);
    result.progress = { phase: 'route', checkpoint: index, laps: progress.laps, requested };
  }
}

module.exports = { observeWall, surface, validateRoute, runRoute };
