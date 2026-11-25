const { surface } = require('./partner-route.cjs');
const { waterBlock } = require('./self-vitals.cjs');

const sides = [[1,0], [-1,0], [0,1], [0,-1]];
const directions = [...sides.map(([x,z]) => [x,0,z]), [0,1,0], [0,-1,0]];

// 只用覆盖整格的碰撞体证明封闭，局部形状交给只读寻路核对。
function fullCube(block) {
  return block.shapes?.some(([minX,minY,minZ,maxX,maxY,maxZ]) =>
    minX <= 0 && minY <= 0 && minZ <= 0 && maxX >= 1 && maxY >= 1 && maxZ >= 1);
}

// 从本人的水中脚格查附近干岸，把地形时点和寻路结论挂在同一份余气回执上。
// 步骤：绑定本人采样 → 枚举有界相通空间及干岸候选 → 只读核验路径或给出受阻与缺证原因。
function observeShore(bot, vitals, probe) {
  const from = bot.entity.position;
  const center = from && [from.x,from.y,from.z].every(Number.isFinite) ? from.floored() : null;
  const coverage = { origin: center && {x:center.x,y:center.y,z:center.z}, radius:4, below:1, above:3,
    inspected:0, unknown:0, candidates:0, boundaryReached:false };
  const evidence = { entityId:bot.entity.id, sampledAtMs:Math.floor(performance.now()), coverage };
  const outcome = (status, reason, extra) => ({ ...evidence, status, reason, ...extra });
  if (vitals.air.state !== 'sampled') return outcome('unconfirmed','air_unsampled');
  evidence.airSampledAtMs = vitals.air.sampledAtMs;
  if (!center) return outcome('unconfirmed','position_unknown');
  const feet = bot.blockAt(center);
  if (!feet) return outcome('unconfirmed','terrain_unknown');
  if (!waterBlock(feet) && !bot.entity.isInWater) return outcome('unconfirmed','not_in_water');
  if (fullCube(feet)) return outcome('unconfirmed','actor_cell_conflict');

  const inside = p => Math.abs(p.x-center.x) <= coverage.radius && Math.abs(p.z-center.z) <= coverage.radius &&
    p.y >= center.y-coverage.below && p.y <= center.y+coverage.above;
  if (!vitals.air.waterCell || !inside(vitals.air.waterCell)) return outcome('unconfirmed','air_location_unconfirmed');
  const pending = [center], visited = new Set(), stands = [];
  for (let index = 0; index < pending.length; ++index) {
    const p = pending[index], key = `${p.x},${p.y},${p.z}`;
    if (visited.has(key)) continue;
    visited.add(key);
    ++coverage.inspected;
    const block = bot.blockAt(p);
    if (!block) { ++coverage.unknown; continue; }
    if (fullCube(block)) continue;

    const standing = surface(bot,p);
    if (standing === '?') ++coverage.unknown;
    if (standing === '.') {
      const adjacent = sides.flatMap(([x,z]) => [bot.blockAt(p.offset(x,0,z)),bot.blockAt(p.offset(x,-1,z))]);
      if (adjacent.some(neighbor => waterBlock(neighbor) && !fullCube(neighbor))) stands.push(p);
      else if (adjacent.some(neighbor => !neighbor)) ++coverage.unknown;
    }
    for (const [x,y,z] of directions) {
      const neighbor = p.offset(x,y,z);
      if (inside(neighbor)) pending.push(neighbor);
      else coverage.boundaryReached = true;
    }
  }
  coverage.candidates = stands.length;
  if (!stands.length) {
    if (coverage.unknown) return outcome('unconfirmed','terrain_unknown');
    if (!coverage.boundaryReached) return outcome('blocked','sealed_volume');
    return outcome('unconfirmed','no_bank_in_range');
  }
  let route;
  try { route = probe(from,stands,inside); }
  catch { return outcome('unconfirmed','path_unconfirmed'); }
  const end = route?.path?.at(-1);
  if (route?.status === 'success' && end && stands.some(p => p.equals(end)) &&
      route.path.every(step => inside(step) && !step.toBreak?.length && !step.toPlace?.length))
    return outcome('reachable','path_verified',{ stand:{x:end.x,y:end.y,z:end.z} });
  if (route?.status === 'noPath' && !coverage.unknown) return outcome('blocked','no_path_in_range');
  return outcome('unconfirmed',coverage.unknown ? 'terrain_unknown' : 'path_unconfirmed');
}

module.exports = { observeShore };
