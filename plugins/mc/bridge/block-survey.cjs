const symbols = '0123456789abcdefghijklmnopqrstuvwxyz';

// 区分实际空格、未加载格和有限调色表，保持扫描结果可按坐标还原。
function blockToken(block, palette) {
  if (!block) return '?';
  if (['air', 'cave_air', 'void_air'].includes(block.name)) return '.';
  let token = palette.get(block.name);
  if (!token && palette.size < symbols.length) {
    token = symbols[palette.size];
    palette.set(block.name, token);
  }
  return token ?? '*';
}

// 读取玩家或角色附近已加载的格子，输出从西北底层开始的紧凑体积。
// 步骤：校验扫描中心 → 按层和行采样 → 交付坐标原点、调色表和网格。
function blockSurvey(bot, center, depthBelow = 1) {
  if (!center || center.distanceTo(bot.entity.position) > 24) throw new Error('观察中心不在当前角色可检查的范围内');
  const origin = center.floored().offset(-5, -depthBelow, -5);
  const palette = new Map();
  const layers = [];
  for (let dy = 0; dy < depthBelow + 3; ++dy) {
    const rows = [];
    for (let dz = 0; dz < 11; ++dz) {
      let row = '';
      for (let dx = 0; dx < 11; ++dx) row += blockToken(bot.blockAt(origin.offset(dx, dy, dz)), palette);
      rows.push(row);
    }
    layers.push(rows);
  }
  return { origin, palette: Object.fromEntries([...palette].map(([name, key]) => [key, name])), layers };
}

// 从同一份网格提取覆盖边界和未加载格数，供目标摘要引用。
function surveyCoverage({ origin, layers }) {
  let unknown = 0;
  for (const rows of layers) for (const row of rows) unknown += row.split('?').length - 1;
  return { origin, width: layers[0][0].length, height: layers.length, depth: layers[0].length, unknown };
}

// 将同一扇门的观察归到下半格，缺失或不相配的半扇保留为未确认。
// 步骤：从命中的半格定位下半格 → 读回另一半并核对门的状态 → 按下半格去重。
function observedDoors(bot, blocks) {
  const doors = new Map();
  for (const block of blocks) {
    const state = block.getProperties();
    if (state.half !== 'lower' && state.half !== 'upper') continue;
    const lower = block.position.offset(0, state.half === 'upper' ? -1 : 0, 0);
    const id = `${lower.x},${lower.y},${lower.z}`;
    if (doors.has(id)) continue;
    const other = bot.blockAt(block.position.offset(0, state.half === 'upper' ? -1 : 1, 0));
    const otherState = other?.name === block.name ? other.getProperties() : null;
    const paired = otherState && (state.half === 'lower' ? otherState.half === 'upper' : otherState.half === 'lower') &&
      otherState.facing === state.facing && otherState.hinge === state.hinge;
    const door = { id, block: block.name, status: paired ? 'complete' : 'incomplete', [state.half]: block.position.clone() };
    if (paired) door[otherState.half] = other.position.clone();
    doors.set(id, door);
  }
  return [...doors.values()];
}

module.exports = { blockSurvey, surveyCoverage, observedDoors };
