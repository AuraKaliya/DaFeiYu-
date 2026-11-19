// 将实际位置或视线命中的方块划入四格区域。
function areaOf(point) {
  return [point.x, point.y, point.z].map(n => Math.floor(n / 4)).join(',');
}

// 按离开再进入计数，保留最近三次返回期间真正首次看到的区域。
// 步骤：在连续采样中收集视线命中 → 记录出入区域 → 生成有界的观察摘要。
function exploration(order) {
  let window = 1, sample = 0, previous = null, report = null, lastTime = null;
  const areas = new Map(), seen = new Set();
  // 中断或容量溢出后从新窗口开始，不能把旧轨迹拼接成受阻证据。
  function reset() {
    ++window; previous = report = lastTime = null; areas.clear(); seen.clear();
  }
  return {
    reset,
    // 只在真实移动采样时推进序号；同一区域内多次读取不增加返回次数。
    // 步骤：检查采样连续性 → 记录可见区域 → 关联离开与重返的水位。
    observe(area, surface, time) {
      if (lastTime !== null && (time < lastTime || time - lastTime > 1000)) reset();
      if ((!areas.has(area) && areas.size >= 128) || (surface && !seen.has(surface) && seen.size >= 256)) reset();
      ++sample; lastTime = time;
      if (area !== previous && previous !== null) areas.get(previous).left = seen.size;
      if (surface) seen.add(surface);
      if (area === previous) return;
      let record = areas.get(area);
      if (!record) { record = { left: 0, trips: [] }; areas.set(area, record); }
      else {
        record.trips.push({ sample, before: record.left });
        if (record.trips.length > 3) record.trips.shift();
        if (record.trips.length === 3) report = { area, trips: [...record.trips] };
      }
      previous = area;
    },
    // 只在采样仍新鲜时交付事实，查询本身不刷新来源。
    read(time) {
      if (!report || lastTime === null || time < lastTime || time - lastTime > 1000) return null;
      return { order, window, sample, area: report.area, returns: report.trips.map(trip => trip.sample),
        gained: seen.size - report.trips[0].before, seen: seen.size };
    },
  };
}

// 在角色已加载的视线范围内取样；缺块、校正移动或停顿都不能证明连续探索。
function sampleView(bot, tracker, now) {
  const head = bot.entity.position.offset(0, bot.entity.eyeHeight, 0);
  for (const x of [-8, 8]) for (const z of [-8, 8])
    if (!bot.blockAt(head.offset(x, 0, z), false)) { tracker.reset(); return; }
  const block = bot.blockAtCursor(8);
  tracker.observe(areaOf(bot.entity.position), block ? areaOf(block.position) : null, now);
}
module.exports = { exploration, sampleView };
