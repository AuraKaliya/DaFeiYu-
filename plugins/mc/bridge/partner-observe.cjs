const { setTimeout: delay } = require('node:timers/promises');
const { playerFocus } = require('./player-focus.cjs');
const { belowFeet } = require('./world.cjs');

const symbols = '0123456789abcdefghijklmnopqrstuvwxyz';
const air = new Set(['air', 'cave_air', 'void_air']);

// 报告具体观察参数错误，供子 Agent 按原场景调整一次。
function invalidObservation(code, message) {
  return Object.assign(new Error(message), { code });
}

// 选择观察用途，并兼容旧调用通过参数表达的用途。
// 步骤：识别窗口字段和原话来源 → 排除搜索与窗口冲突 → 统一窗口来源。
function observationInput(args) {
  const window = ['sources', 'radius', 'detail', 'samples', 'verify'].some(key => Object.hasOwn(args, key));
  const mode = args.mode ?? (window ? 'scene' : args.message !== undefined || args.focus ? 'pointed' :
    args.around || args.block ? 'search' : 'summary');
  if (mode === 'wall') {
    const radius = args.radius ?? 8;
    if (!Number.isSafeInteger(args.message) || args.message <= 0 || !Number.isSafeInteger(radius) || radius < 5 || radius > 13 ||
      Object.keys(args).some(key => !['mode', 'message', 'radius'].includes(key)))
      throw invalidObservation('observe_wall_parameters', '墙体观察需要原话 message，可选半径 5～13 格');
    return { mode, options: { message: args.message, radius } };
  }
  if (mode === 'route') {
    if (typeof args.observation !== 'string' || (args.wall !== undefined && typeof args.wall !== 'string') ||
      (args.waypoints !== undefined && (!Array.isArray(args.waypoints) || args.waypoints.length !== 4)) ||
      Object.keys(args).some(key => !['mode', 'observation', 'wall', 'waypoints'].includes(key)))
      throw invalidObservation('observe_route_parameters', '路线验证需要墙体观察编号，可选墙体候选与四个绕行站位');
    return { mode, options: args };
  }
  if (mode === 'scene') {
    if (args.block !== undefined)
      throw invalidObservation('observe_mixed_modes', '多来源观察使用 mode=scene，查找指定材料使用 mode=search 和 block');
    const sources = args.sources ?? (args.message !== undefined ? (args.verify?.length ? ['message', 'self'] : ['message']) :
      [args.around ?? 'self']);
    if (args.around && (args.message !== undefined || (Array.isArray(args.sources) && !args.sources.includes(args.around))))
      throw invalidObservation('observe_mixed_modes', '多来源观察使用 sources，原话使用 message 来源；请去掉 around');
    const { around, ...options } = args;
    return { mode, options: { ...options, sources } };
  }
  if (window || (mode === 'pointed' && args.around) ||
    (mode === 'search' && (args.message !== undefined || args.focus)) ||
    (mode === 'pointed' && args.mode && args.block))
    throw invalidObservation('observe_mixed_modes', '原话指向用 mode=pointed 和 message；场景窗口用 mode=scene；查材料用 mode=search');
  if (mode === 'pointed' && (!Number.isSafeInteger(args.message) || args.message <= 0))
    throw invalidObservation('observe_message_required', '原话指向需要原玩家消息号 message');
  return { mode, options: args };
}

// 对每一帧合并相交区域，网格中的等号引用同帧更早中心的同一坐标。
// 步骤：校验中心和覆盖区 → 按坐标去重读取 → 输出来源及可选网格。
function surveyView(bot, view, position, radius, cells, palette, detail) {
  if (!position || position.distanceTo(bot.entity.position) > 24) {
    view.status = 'unavailable';
    return;
  }
  const origin = position.floored().offset(-radius, -4, -radius);
  const width = radius * 2 + 1;
  view.coverage = { origin, width, depth: width, height: 7, method: 'loaded_blocks' };
  let grid = '';
  for (let dy = 0; dy < 7; ++dy) for (let dz = 0; dz < width; ++dz) for (let dx = 0; dx < width; ++dx) {
    const point = origin.offset(dx, dy, dz);
    const key = `${point.x},${point.y},${point.z}`;
    if (cells.has(key)) { grid += '='; continue; }
    const block = bot.blockAt(point);
    const name = air.has(block?.name) ? 'air' : block?.name ?? null;
    cells.set(key, name);
    if (name == null) { grid += '?'; continue; }
    if (name === 'air') { grid += '.'; continue; }
    if (!palette.has(name) && palette.size < symbols.length) palette.set(name, symbols[palette.size]);
    grid += palette.get(name) ?? '*';
  }
  if (detail === 'cells') view.grid = grid;
}

// 读取已知重叠区域的实际变化，未加载和离开观察窗口属于覆盖变化。
function compareCells(prior, current) {
  const changes = [];
  let total = 0;
  for (const [key, after] of current) {
    const before = prior.get(key);
    if (before == null || after == null || before === after) continue;
    ++total;
    if (changes.length < 12) changes.push({ cell: key.split(',').map(Number), before, after });
  }
  return { total, changes };
}

// 从角色和配对玩家实体提取本次可读状态，缺失字段保持未知。
function actorState(entity) {
  if (!entity?.position) return null;
  return { position: entity.position.clone(), yaw: entity.yaw, pitch: entity.pitch, inWater: entity.isInWater };
}

// 从同一帧装配实时状态、原话来源和已加载方块。
// 步骤：取当前人物状态 → 解析中心 → 合并网格及候选 → 汇总实际变化。
function takeSample(bot, options, player, messageAnchor, inventory, prior) {
  const cells = new Map(), palette = new Map(), readings = new Map(), views = [];
  const world = { entity: bot.entity,
    // 视线分析与多中心网格共用同一帧的逐格读取。
    blockAt(point) {
      const key = `${point.x},${point.y},${point.z}`;
      if (!readings.has(key)) readings.set(key, bot.blockAt(point));
      return readings.get(key);
    },
  };
  for (const source of options.sources) {
    const view = { source, status: 'sampled' };
    views.push(view);
    let center;
    if (source === 'message') {
      view.message = options.message;
      let original;
      try { original = messageAnchor(options.message); }
      catch { view.status = 'unavailable'; continue; }
      view.anchor = original.anchor;
      center = original.center;
    } else {
      const actor = source === 'self' ? bot.entity : player;
      view.actor = actorState(actor);
      center = actor?.position;
      if (source === 'self') Object.assign(view.actor, { health: bot.health, food: bot.food, inventory: inventory() });
    }
    surveyView(world, view, center, options.radius, cells, palette, options.detail);
    if (source !== 'message' && view.status === 'sampled') {
      const { x, y, z } = belowFeet(center);
      view.belowFeet = { x, y, z, block: cells.get(`${x},${y},${z}`) ?? null };
    }
    if (source === 'message' && view.status === 'sampled') view.playerFocus = playerFocus(world, view.anchor, center, options.focus,
      { player_nearby: options.sources.includes('player') ? player?.position : undefined,
        self_nearby: options.sources.includes('self') ? bot.entity.position : undefined });
  }
  const counts = new Map();
  let unknown = 0;
  for (const name of cells.values()) {
    if (name == null) ++unknown;
    else counts.set(name, (counts.get(name) ?? 0) + 1);
  }
  const blocks = Object.fromEntries([...counts].sort((a, b) => b[1] - a[1]).slice(0, 12));
  const sample = { sampledAtMs: Math.floor(performance.now()), views, coverage: { unique: cells.size, unknown }, blocks };
  if (options.verify.length) {
    const own = views.find(view => view.source === 'self')?.coverage;
    sample.verified = options.verify.map(({ x, y, z }) => ({ x, y, z, block: cells.get(`${x},${y},${z}`) ?? null,
      atSelf: !!own && x >= own.origin.x && x < own.origin.x + own.width && y >= own.origin.y &&
        y < own.origin.y + own.height && z >= own.origin.z && z < own.origin.z + own.depth }));
  }
  if (options.detail === 'cells') sample.palette = Object.fromEntries([...palette].map(([name, symbol]) => [symbol, name]));
  if (prior) sample.delta = compareCells(prior, cells);
  return { sample, cells };
}

// 以有限次数观察同一世界，等待期间不占用角色输入。
// 步骤：校验来源及预算 → 分次采样并记录时间 → 按传输预算提交前缀。
async function observeWindow(bot, args, result, context) {
  const { sources = ['self'], radius = 3, detail = 'summary', message, verify = [], focus } = args;
  const { count = 1, intervalMs = 250 } = args.samples ?? {};
  if (!Array.isArray(sources) || !sources.length || sources.length > 3 || new Set(sources).size !== sources.length ||
    sources.some(source => !['self', 'player', 'message'].includes(source)) ||
    sources.includes('message') !== (Number.isSafeInteger(message) && message > 0) ||
    (focus !== undefined && (!sources.includes('message') || !['hit', 'openings', 'objects'].includes(focus))))
    throw invalidObservation('observe_invalid_sources', 'sources 只能包含 self、player、message；使用 message 来源时须传原话编号');
  if (!Number.isInteger(radius) || radius < 1 || radius > 5 || !['summary', 'cells'].includes(detail) ||
    !Number.isInteger(count) || count < 1 || count > 3 || !Number.isInteger(intervalMs) || intervalMs < 100 || intervalMs > 1000 ||
    !Array.isArray(verify) || verify.length > 16 || verify.some(cell => !cell || ![cell.x, cell.y, cell.z].every(Number.isSafeInteger)))
    throw invalidObservation('observe_invalid_window', '无效采样预算：半径 1～5 格，1～3 帧，间隔 100～1000 ms，最多验 16 格');
  const { check, player, messageAnchor, inventory, signal, id, generation } = context;
  const dimension = bot.game.dimension;
  result.observation = { id, generation, dimension, requested: count, samples: [] };
  let cells;
  for (let i = 0; i < count; ++i) {
    if (i) await delay(intervalMs, undefined, { signal });
    check();
    if (bot.game.dimension !== dimension) throw new Error('观察期间世界维度变更');
    const frame = takeSample(bot, { sources, radius, detail, message, verify, focus }, player(), messageAnchor, inventory, cells);
    result.observation.samples.push(frame.sample);
    if (Buffer.byteLength(JSON.stringify(result)) > 7500) {
      result.observation.samples.pop();
      if (i === 0) throw invalidObservation('observe_window_too_large', '观察超出回执范围，请缩小半径或选择摘要');
      result.observation.truncated = true;
      return;
    }
    cells = frame.cells;
  }
}

module.exports = { observationInput, observeWindow };
