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

module.exports = { blockSurvey };
