// 扫描区分空气与不可读的格子。
const air = block => block && ['air', 'cave_air', 'void_air'].includes(block.name);
// 墙体侧面和支撑都需要实心方块。
const solid = block => block?.boundingBox === 'block';
const axes = [[1, 0], [0, 1]];

// 读取玩家发送原话时的眼位及朝向。
function look(bot, anchor) {
  if (!anchor.eye || !anchor.facing) return null;
  const eye = bot.entity.position.clone().set(anchor.eye.x, anchor.eye.y, anchor.eye.z);
  const { yaw, pitch } = anchor.facing;
  return { eye, direction: [-Math.sin(yaw) * Math.cos(pitch), Math.sin(pitch), -Math.cos(yaw) * Math.cos(pitch)] };
}

// 逐格跨过射线碰到的边界，未知或非空气格会终止读数。
// 步骤：计算三轴起始边界 → 读取当前体素 → 沿最近的边界推进。
function traceLine(bot, eye, direction, limit) {
  const crossed = new Set(), names = ['x', 'y', 'z'];
  const cell = eye.floored();
  const delta = direction.map(value => value === 0 ? Infinity : 1 / Math.abs(value));
  const next = names.map((axis, i) => direction[i] === 0 ? Infinity :
    (cell[axis] + Number(direction[i] > 0) - eye[axis]) / direction[i]);
  let distance = 0;
  while (distance <= limit) {
    const { x, y, z } = cell;
    const block = bot.blockAt(cell.clone());
    if (!block) return { crossed, unavailable: true };
    if (!air(block)) return { crossed, hit: { x, y, z, block: block.name } };
    crossed.add(`${x},${y},${z}`);
    distance = Math.min(...next);
    for (let i = 0; i < names.length; ++i) if (next[i] === distance) {
      cell[names[i]] += Math.sign(direction[i]);
      next[i] += delta[i];
    }
  }
  return { crossed };
}

// 合并原话视线、视锥、命中格邻域和双方周围的已加载方块。
// 步骤：读取准星命中 → 核对候选的视线与来源 → 去重并按证据强度截取。
function objectFocus(bot, anchor, nearby) {
  const view = look(bot, anchor);
  if (!view) return { status: 'unavailable', hit: null, objects: [] };
  const { eye, direction } = view;
  const ray = traceLine(bot, eye, direction, 6);
  const items = new Map();
  let unknown = ray.unavailable ? 1 : 0;
  const key = ({ x, y, z }) => `${x},${y},${z}`;
  const inspect = point => {
    const id = key(point);
    if (items.has(id)) return items.get(id);
    const block = bot.blockAt(point);
    const value = !block || air(block) ? null : { id, x: point.x, y: point.y, z: point.z, block: block.name, sources: [] };
    items.set(id, value);
    return value;
  };
  const origin = eye.floored();
  for (let dy = -6; dy <= 6; ++dy) for (let dz = -6; dz <= 6; ++dz) for (let dx = -6; dx <= 6; ++dx) {
    const point = origin.offset(dx, dy, dz);
    const toward = [point.x + 0.5 - eye.x, point.y + 0.5 - eye.y, point.z + 0.5 - eye.z];
    const distance = Math.hypot(...toward);
    const cosine = distance ? toward.reduce((dot, value, i) => dot + value * direction[i], 0) / distance : 1;
    const direct = ray.hit && key(ray.hit) === key(point);
    const neighbor = ray.hit && !direct && Math.max(Math.abs(ray.hit.x - point.x), Math.abs(ray.hit.y - point.y),
      Math.abs(ray.hit.z - point.z)) <= 1 && cosine > 0;
    const cone = distance <= 6 && cosine >= 0.87;
    if (!direct && !cone && !neighbor) continue;
    const check = direct ? ray : traceLine(bot, eye, toward.map(value => value / distance), distance);
    if (check.unavailable) ++unknown;
    if (!check.hit || key(check.hit) !== key(point)) continue;
    const item = inspect(point);
    if (direct) item.sources.push('ray');
    if (cone) item.sources.push('cone');
    if (neighbor) item.sources.push('hit_neighbor');
    item.visible = true;
    const below = bot.blockAt(point.offset(0, -1, 0));
    if (below && !air(below)) item.below = below.name;
    item.angle = Math.round(Math.acos(Math.min(1, cosine)) * 180 / Math.PI);
    item.distance = Math.round(distance * 10) / 10;
  }
  for (const [source, position] of Object.entries(nearby)) {
    if (!position || position.distanceTo(bot.entity.position) > 24) continue;
    const point = position.floored();
    for (let dy = -1; dy <= 2; ++dy) for (let dz = -2; dz <= 2; ++dz) for (let dx = -2; dx <= 2; ++dx) {
      const item = inspect(point.offset(dx, dy, dz));
      if (item) {
        item.sources.push(source);
        item.distance ??= Math.round(eye.distanceTo(point.offset(dx + 0.5, dy + 0.5, dz + 0.5)) * 10) / 10;
      }
    }
  }
  const ranked = [...items.values()].filter(Boolean).sort((a, b) =>
    Number(b.sources.includes('ray')) - Number(a.sources.includes('ray')) ||
    Number(!!b.visible) - Number(!!a.visible) || (a.angle ?? 180) - (b.angle ?? 180) || a.distance - b.distance);
  const counts = new Map();
  const objects = ranked.filter(item => {
    const count = counts.get(item.block) ?? 0;
    counts.set(item.block, count + 1);
    return count < 4;
  }).slice(0, 16);
  return { status: objects.length ? 'observed' : unknown ? 'unavailable' : 'none',
    hit: ray.hit ?? null, objects, coverage: { unknownRays: unknown, omitted: ranked.length - objects.length } };
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
function playerFocus(bot, anchor, center, focus = 'openings', nearby = {}) {
  if (focus === 'objects') return objectFocus(bot, anchor, nearby);
  const view = look(bot, anchor);
  const { crossed, hit, unavailable } = view ? traceLine(bot, view.eye, view.direction, 6) :
    { crossed: new Set(), unavailable: true };
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
