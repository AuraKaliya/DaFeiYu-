const assert = require('node:assert/strict');
const { test } = require('node:test');
const { observationInput, observeWindow } = require('../../plugins/mc/bridge/partner-observe.cjs');
const { observeWall, validateRoute, runRoute } = require('../../plugins/mc/bridge/partner-route.cjs');
const { matchesDigReference, collectionProgress, digAndReadback } = require('../../plugins/mc/bridge/collection.cjs');
const { playerFocus } = require('../../plugins/mc/bridge/player-focus.cjs');
const { monitorMove } = require('../../plugins/mc/bridge/move-progress.cjs');
const { observeDrops } = require('../../plugins/mc/bridge/dropped-items.cjs');
const { trackBreakDrop, pickupShortfall } = require('../../plugins/mc/bridge/collect-drops.cjs');
const { inventoryCapacity } = require('../../plugins/mc/bridge/inventory-capacity.cjs');
const { EventEmitter } = require('node:events');

class Position {
  // 保存本次测试的世界坐标。
  constructor(x, y, z) { this.set(x, y, z); }
  // 替换当前值并返回供原话视线使用的位置。
  set(x, y, z) { Object.assign(this, { x, y, z }); return this; }
  // 复制当前值，模拟游戏取样脱离实时实体。
  clone() { return this.offset(0, 0, 0); }
  // 提供格子附近相对坐标。
  offset(x, y, z) { return new Position(this.x + x, this.y + y, this.z + z); }
  // 将实体位置落在方块坐标上。
  floored() { return new Position(...[this.x, this.y, this.z].map(Math.floor)); }
  // 返回两个实际采样中心的距离。
  distanceTo(p) { return Math.hypot(this.x - p.x, this.y - p.y, this.z - p.z); }
}

// 提供互相覆盖的两个实时来源和固定原话中心。
function fixture() {
  const origin = new Position(0, -60, 0);
  const player = { position: origin.offset(1, 0, 0) };
  const reads = new Map();
  const bot = { game: { dimension: 'overworld' }, entity: { position: origin }, health: 20, food: 18,
    // 模拟不同帧中的实际改变和不可读取区域。
    blockAt({ x, y, z }) {
      const key = `${x},${y},${z}`;
      reads.set(key, (reads.get(key) ?? 0) + 1);
      if (x === -1) return null;
      return { name: x === 0 && y === -60 && z === 0 && player.position.x === 1 ? 'dirt' : 'air' };
    },
  };
  const context = { id: 'probe', generation: 'world', player: () => player, inventory: () => ({ dirt: 4 }),
    messageAnchor: () => ({ anchor: { position: origin, dimension: 'overworld', sampledAtMs: 42 }, center: origin }), check: () => {} };
  return { bot, player, context, reads };
}

// 用缺读、异物和同距离的实体核对只读掉落观察的覆盖及顺序。
// 步骤：布置带身份的掉落 → 对照过滤前后快照 → 验证截断和参数边界。
test('drop observation distinguishes filtered stacks, missing data and bounded coverage', () => {
  const { bot } = fixture();
  const center = bot.entity.position;
  const drop = (id, x, name = 'dirt') => ({ id, name: 'item', position: center.offset(x, 0, 0),
    getDroppedItem: () => ({ name, count: 2 }) });
  bot.registry = { itemsByName: { dirt: {} } };
  bot.entities = { a: drop(4, 2), b: drop(2, 2), c: drop(6, 1, 'stone'), far: drop(8, 5),
    waiting: { ...drop(10, 1), getDroppedItem() { throw new Error('metadata pending'); } },
    unseen: { id: 12, name: 'item' }, player: { name: 'player', position: center }, gone: { ...drop(13, 1), isValid: false } };
  const options = observationInput({ mode: 'drops', item: 'minecraft:dirt', radius: 2 }).options;
  const sample = observeDrops(bot, options, 'work', 'world');
  assert.deepEqual(sample.entities.map(e => [e.entityId, e.item, e.count]), [[2, 'dirt', 2], [4, 'dirt', 2]]);
  assert.deepEqual([sample.unknownStack, sample.unlocated, sample.omitted, sample.item], [1, 1, 0, 'dirt']);
  center.x = 0.5;
  assert.equal(sample.position.x, 0);
  const unfiltered = observeDrops(bot, observationInput({ mode: 'drops' }).options, 'work2', 'world');
  assert.equal(unfiltered.entities.find(e => e.entityId === 10).count, null);
  for (let i = 20; i < 40; ++i) bot.entities[i] = drop(i, 0);
  const crowded = observeDrops(bot, options, 'work3', 'world');
  assert.deepEqual([crowded.entities.length, crowded.omitted], [12, 10]);
  assert.throws(() => observeDrops(bot, { radius: 1, item: 'absent' }, 'work', 'world'), { code: 'observe_drop_item_unknown' });
  for (const args of [{ mode: 'drops', radius: 17 }, { mode: 'drops', item: '' }, { mode: 'drops', message: 2 }, { item: 'dirt' }])
    assert.throws(() => observationInput(args), { code: 'observe_drop_parameters' });
  delete bot.entity.position;
  assert.throws(() => observeDrops(bot, options, 'work', 'world'), { code: 'observe_drop_position_unavailable' });
});

// 验证库存空间只包括主背包与快捷栏，缺读和自定义堆叠保留未知。
test('inventory capacity separates empty and merge space from unconfirmed or excluded slots', () => {
  const dirt = { name: 'dirt', count: 62 }, stone = { name: 'stone', count: 5 };
  const bot = { registry: { itemsByName: { dirt: { stackSize: 64 } } },
    inventory: { inventoryStart: 1, inventoryEnd: 5, slots: [null, null, dirt, stone, { ...dirt, count: 64 }, null] } };
  const fields = () => {
    const { status, slots, emptySlots, mergeSpace, knownSpace, unknownSlots } = inventoryCapacity(bot, 'dirt');
    return [status, slots, emptySlots, mergeSpace, knownSpace, unknownSlots];
  };
  assert.deepEqual(fields(), ['sampled', 4, 1, 2, 66, 0]);
  bot.inventory.slots[1] = stone;
  dirt.count = 64;
  assert.deepEqual(fields(), ['sampled', 4, 0, 0, 0, 0]);
  bot.inventory.slots[3] = { ...dirt, components: [{ type: 'custom_name' }] };
  delete bot.inventory.slots[4];
  assert.deepEqual(fields(), ['partial', 4, 0, 0, 0, 2]);
  bot.inventory.inventoryEnd = 8;
  assert.equal(inventoryCapacity(bot, 'dirt').status, 'unavailable');
});

// 核对采掘附近新堆的部分拾取、其他角色拾取、看丢和实体编号复用。
// 步骤：排除旧堆与异物 → 注入实际数量与消失事件 → 检查同号换实体和监听回收。
test('pickup evidence follows one new stack and credits only protocol collection events', () => {
  const bot = new EventEmitter(), point = new Position(2, -61, 0), result = {};
  bot._client = new EventEmitter();
  bot.entity = { id: 1, uuid: 'self' };
  bot.game = { dimension: 'overworld' };
  const drop = (id, item) => ({ id, name: 'item', uuid: 'stack-' + id, position: point.clone(),
    getDroppedItem: () => ({ name: item, count: 5 }) });
  const old = drop(3, 'dirt'), other = { id: 2, uuid: 'other' };
  bot.entities = { 1: bot.entity, 2: other, 3: old };
  const observer = trackBreakDrop(bot, point, 'dirt', result);
  bot.emit('itemDrop', old);
  bot.emit('itemDrop', bot.entities[4] = drop(4, 'stone'));
  bot.entities[5] = { ...drop(5, 'dirt'), position: point.offset(9, 0, 0) };
  assert.equal(observer.live(), undefined);
  const picked = bot.entities[6] = drop(6, 'dirt');
  bot.emit('itemDrop', picked);
  const collect = (id, collector, count) => bot._client.emit('collect',
    { collectedEntityId: id, collectorEntityId: collector, pickupItemCount: count });
  collect(6, 1, 2);
  collect(6, 2, 1);
  collect(3, 1, 5);
  const entry = observer.entry;
  assert.deepEqual([entry.selfCount, entry.otherCount, entry.lastCollector.uuid, entry.state], [2, 1, 'other', 'visible']);
  bot.emit('entityGone', picked);
  bot.entities[6] = drop(6, 'dirt');
  collect(6, 1, 2);
  observer.stop();
  assert.deepEqual([entry.state, result.pickups.selfCollected, result.pickups.otherCollected], ['left_view', { dirt: 2 }, { dirt: 1 }]);
  assert.deepEqual([bot.listenerCount('itemDrop'), bot.listenerCount('entityGone'), bot._client.listenerCount('collect')], [0, 0, 0]);
  for (let i = 10; i < 15; ++i) {
    const watcher = trackBreakDrop(bot, point, 'dirt', result);
    bot.emit('itemDrop', bot.entities[i] = drop(i, 'dirt'));
    delete bot.entities[i];
    watcher.stop();
    assert.deepEqual([watcher.entry.state, watcher.entry.selfCount, watcher.entry.otherCount], ['left_view', 0, 0]);
  }
  assert.deepEqual([result.pickups.attempted, result.pickups.omitted, result.pickups.recent.length], [6, 2, 4]);
});

// 入包没有得到双重读回时，保留现场能够支持的具体受阻原因。
test('missing pickup reports the observed collector, visibility or missing inventory update', () => {
  for (const [entry, code] of [[{ state: 'unseen' }, 'pickup_unverified'], [{ state: 'visible' }, 'pickup_still_visible'],
    [{ state: 'left_view' }, 'pickup_lost'], [{ state: 'left_view', otherCount: 1 }, 'pickup_other_collector'],
    [{ state: 'left_view', otherCount: 2, selfCount: 1 }, 'pickup_stock_unconfirmed']])
    assert.equal(pickupShortfall(entry).code, code);
});

// 注入动作中断和迟到的门格更新，验证只按原目标计数并保留连带效果。
// 步骤：修改调用中的对象再抛错 → 读回原两格 → 单独验证只变化连带格时的结果。
test('dig readback preserves original cells and partial effects after interruption', async () => {
  const target = { name: 'oak_door', position: new Position(3, -60, 2) };
  const upper = { name: 'oak_door', position: target.position.offset(0, 1, 0) };
  let reads = 0, notifications = 0;
  const interrupted = new Error('interrupted');
  const result = { broken: 0, changes: [] };
  const bot = { dig: async () => { target.position.x += 1; target.name = 'air'; throw interrupted; },
    blockAt: pos => pos.x !== 3 || (pos.y === -60 && reads++ === 0) ? null : { name: 'air' } };
  await assert.rejects(digAndReadback(bot, target, [target, upper], result, () => ++notifications), error => error === interrupted);
  assert.deepEqual([result.broken, notifications, result.changes.map(c => [c.x, c.y, c.before, c.after])],
    [1, 1, [[3, -59, 'oak_door', 'air'], [3, -60, 'oak_door', 'air']]]);
  const partial = { broken: 0, changes: [] };
  target.name = 'oak_door';
  target.position.x = 3;
  bot.dig = async () => {};
  bot.blockAt = pos => pos.y === -60 ? target : { name: 'air' };
  assert.equal(await digAndReadback(bot, target, [target, upper], partial, () => ++notifications), false);
  assert.deepEqual([partial.broken, partial.changes.length, notifications], [0, 1, 1]);
});

// 对照工作时点、换目标、破坏和库存，核对采掘在途样本的独立读数。
test('collection polls retain the phase start and sample breaks separately from stock', () => {
  let time = 100, stock = 1;
  const result = { broken: 0, stock: { item: 'dirt', required: 3 } };
  const progress = collectionProgress(result, () => stock, () => time);
  const block = { position: new Position(2, -61, 0), name: 'grass_block' };
  progress.enter('digging', block);
  time = 200;
  progress.sample();
  assert.deepEqual([result.progress.phaseSinceMs, result.progress.sampledAtMs, result.progress.broken], [100, 200, 0]);
  result.broken = 1;
  result.pickups = { recent: [{ targetCell: { x: 2, y: -61, z: 0 }, state: 'unseen' }] };
  progress.enter('awaiting_pickup');
  assert.deepEqual(result.progress.pickup, { state: 'unseen' });
  assert.deepEqual([result.progress.targetCell, result.progress.lastBreakAtMs, result.progress.stock.current],
    [{ x: 2, y: -61, z: 0 }, 200, 1]);
  stock = 2;
  time = 300;
  Object.assign(result.pickups.recent[0], { state: 'left_view', entityId: 7, item: 'dirt', selfCount: 1, otherCount: 0 });
  progress.sample();
  assert.deepEqual(result.progress.pickup, { state: 'left_view', entityId: 7, item: 'dirt', selfCount: 1, otherCount: 0 });
  assert.deepEqual([result.progress.stock.current, result.progress.lastBreakAtMs, result.progress.phaseSinceMs], [2, 200, 200]);
  progress.enter('selecting');
  assert.equal(Object.hasOwn(result.progress, 'targetCell'), false);
  assert.equal(Object.hasOwn(result.progress, 'pickup'), false);
});

// 在同一移动上分别注入真实位移、无路、位置未知和核实抵达。
// 步骤：对照位置与路径 → 注入数据缺失并复位 → 核对末次采样与监听回收。
test('movement samples keep path outcomes separate from observed position and arrival', () => {
  const bot = new EventEmitter(), result = {}, goal = {};
  const target = { x: 3, y: -60, z: 0 };
  let time = 100;
  bot.entity = { position: new Position(0.5, -60, 0.5) };
  bot.pathfinder = { goal: {} };
  const monitor = monitorMove(bot, target, goal, result, () => time);
  bot.emit('path_update', { status: 'noPath' });
  monitor.sample();
  assert.equal(result.progress.path, 'unknown');
  bot.pathfinder.goal = goal;
  assert.deepEqual([result.progress.phase, result.progress.distance], ['waiting', 3]);
  bot.emit('path_update', { status: 'success' });
  monitor.sample();
  assert.equal(result.progress.phase, 'waiting');
  bot.entity.position.x += 1;
  time += 40;
  monitor.sample();
  assert.deepEqual([result.progress.phase, result.progress.distance, result.progress.startDistance,
    result.progress.vertical, result.progress.lastMovedAtMs], ['moving', 2, 3, 0, time]);
  time += 501;
  monitor.sample();
  assert.equal(result.progress.phase, 'waiting');
  bot.emit('path_update', { status: 'noPath' });
  monitor.sample();
  assert.equal(result.progress.phase, 'no_path');
  bot.entity = null;
  bot.emit('path_update', { status: 'timeout' });
  monitor.sample();
  assert.deepEqual(result.progress, { phase: 'unknown', path: 'timeout', targetCell: target, sampledAtMs: time });
  bot.entity = { position: new Position(3.5,-60,0.5) };
  monitor.stop(true);
  assert.equal(result.progress.phase, 'arrived');
  assert.equal(result.progress.distance, 0);
  assert.equal(bot.listenerCount('path_update'), 0);
});

// 用原局高度及整数另一侧读取脚下格，核对执行采样采用的坐标。
test('live below-feet cells use the original height on both sides of a block boundary', async () => {
  const { bot, context } = fixture();
  for (const [height, below] of [[-60.000041050085805, -62], [-59.99996, -61]]) {
    bot.entity.position.set(0, height, 0);
    const result = {};
    await observeWindow(bot, { sources: ['self'], radius: 1 }, result, context);
    const view = result.observation.samples[0].views[0];
    assert.equal(view.actor.position.y, height);
    assert.equal(view.belowFeet.y, below);
  }
});

// 两个实时中心重叠时同格只读取一次，来源与玩家状态仍可各自追查。
test('combined survey reads shared coordinates once', async () => {
  const { bot, context, reads } = fixture(), result = {};
  await observeWindow(bot, { sources: ['self', 'player'], radius: 1, detail: 'cells',
    verify: [{ x: 0, y: -60, z: 0 }, { x: 2, y: -60, z: 0 }, { x: 3, y: -60, z: 0 }] }, result, context);
  const [sample] = result.observation.samples;
  assert.equal(sample.coverage.unique, 4 * 3 * 7);
  assert.equal(sample.coverage.unknown, 3 * 7);
  assert.equal(Math.max(...reads.values()), 1);
  assert.equal(sample.views[0].actor.health, 20);
  assert.deepEqual(sample.views[0].actor.inventory, { dirt: 4 });
  assert.equal(sample.views[1].actor.position.x, 1);
  assert.equal(sample.views[1].grid.match(/=/g).length, 2 * 3 * 7);
  assert.deepEqual(sample.verified.map(cell => [cell.block, cell.atSelf]), [['dirt', true], ['air', false], [null, false]]);
});

// 在负坐标区分角色脚下格、其他玩家脚下格及未加载状态。
test('live views read the loaded voxel below each actor', async () => {
  const { bot, player, context } = fixture(), result = {};
  bot.entity.position = new Position(-1.4, -60, -5.41);
  player.position = new Position(-0.45, -60, -4.1);
  bot.blockAt = ({ x, y, z }) => x === -1 ? null :
    { name: x === -2 && y === -61 && z === -6 ? 'grass_block' : 'air' };
  await observeWindow(bot, { sources: ['self', 'player', 'message'], message: 12, radius: 1 }, result, context);
  const [sample] = result.observation.samples;
  assert.deepEqual(sample.views[0].belowFeet, { x: -2, y: -61, z: -6, block: 'grass_block' });
  assert.deepEqual(sample.views[1].belowFeet, { x: -1, y: -61, z: -5, block: null });
  assert.equal(sample.views[2].belowFeet, undefined);
});

// 玩家移动后，原话中心保持不动，变化只比较前后都可读的格子。
test('sample series keeps original message apart from live actors', async () => {
  const { bot, player, context, reads } = fixture(), result = {};
  let frame = 0;
  context.check = () => { if (++frame === 2) player.position = player.position.offset(1, 0, 0); };
  await observeWindow(bot, { sources: ['message', 'player', 'self'], message: 12, radius: 1,
    samples: { count: 2, intervalMs: 100 } }, result, context);
  const [first, second] = result.observation.samples;
  assert.ok(second.sampledAtMs - first.sampledAtMs >= 90);
  assert.equal(second.views[0].message, 12);
  assert.equal(second.views[0].anchor.sampledAtMs, 42);
  assert.equal(second.views[0].anchor.position.x, 0);
  assert.equal(second.views[1].actor.position.x, 2);
  assert.equal(second.views[2].actor.position.x, 0);
  assert.equal(reads.get('0,-60,0'), 2);
  assert.deepEqual(second.delta, { total: 1, changes: [{ cell: [0, -60, 0], before: 'dirt', after: 'air' }] });
});

// 停止时保留首帧，定时等待可以取消，非法预算不会开始观察。
test('bounded series can be cancelled before the next sample', async () => {
  const { bot, context } = fixture(), result = {}, abort = new AbortController();
  await assert.rejects(observeWindow(bot, { samples: { count: 4 } }, {}, context), /预算/);
  const pending = observeWindow(bot, { samples: { count: 3, intervalMs: 1000 } }, result, { ...context, signal: abort.signal });
  abort.abort();
  await assert.rejects(pending, { name: 'AbortError' });
  assert.equal(result.observation.samples.length, 1);
});

// 原话与自查参数可合并，材料搜索和多来源采样的混用返回可修复原因。
test('observation modes preserve the original source and explain invalid fields', async () => {
  const { bot, context, reads } = fixture();
  assert.deepEqual(observationInput({ message: 12, radius: 1, verify: [{}] }).options.sources, ['message', 'self']);
  assert.deepEqual(observationInput({ around: 'player', detail: 'summary' }).options.sources, ['player']);
  assert.equal(observationInput({ mode: 'search', around: 'player', block: 'dirt' }).mode, 'search');
  assert.throws(() => observationInput({ block: 'dirt', radius: 3 }), { code: 'observe_mixed_modes' });
  assert.throws(() => observationInput({ mode: 'pointed' }), { code: 'observe_message_required' });
  assert.deepEqual(observationInput({ mode: 'detail', observation: 'original' }).options, { mode: 'detail', observation: 'original' });
  for (const extra of [{}, { observation: '' }, { observation: 'original', message: 2 }])
    assert.throws(() => observationInput({ mode: 'detail', ...extra }), { code: 'observe_detail_parameters' });
  await assert.rejects(observeWindow(bot, { sources: ['message'] }, {}, context), { code: 'observe_invalid_sources' });
  assert.equal(reads.size, 0);
});

// 两面墙同时可见时按原话命中选墙；邻格未加载时保留未知的墙端。
test('wall survey distinguishes the pointed obstacle, both sides and unknown coverage', () => {
  const position = new Position(0.5, -60, -3.5);
  const anchor = { eye: position.offset(0, 1.62, 0), facing: { yaw: Math.PI, pitch: -0.08 } };
  let unknown = false;
  const bot = { entity: { position }, blockAt({ x, y, z }) {
    if (unknown && x === 3 && z === 0) return null;
    const wall = (z === 0 && x >= -2 && x <= 2) || (z === -2 && x >= 4 && x <= 6);
    const stone = y < -60 || (wall && y < -58);
    return { name: stone ? 'stone' : 'air', boundingBox: stone ? 'block' : 'empty' };
  } };
  assert.deepEqual(observationInput({ mode: 'wall', message: 4 }).options, { message: 4, radius: 8 });
  const result = observeWall(bot, anchor, position);
  assert.equal(result.candidates.length, 2);
  assert.equal(result.status, 'selected');
  assert.equal(result.choice, '-2,-60,0:2,0');
  const { origin, rows } = result.coverage;
  assert.equal(rows[-1 - origin.z][-origin.x], '.');
  assert.equal(rows[1 - origin.z][-origin.x], '.');
  unknown = true;
  const partial = observeWall(bot, anchor, position);
  assert.equal(partial.status, 'coverage_required');
  assert.equal(partial.choice, result.choice);
  assert.equal(partial.coverage.unknown, 1);
});

// 固定路径响应只允许在一侧或一端完成每段，局部受阻时检查下一条外绕候选。
test('route validation requires an ordered loop on supported, visible ground', () => {
  const bounds = { minX: -2, maxX: 2, minZ: 0, maxZ: 0, y: -60 };
  const scan = { choice: 'wall', candidates: [{ id: 'wall', complete: true, bounds }],
    coverage: { origin: { x: -6, z: -6 }, rows: Array.from({ length: 13 }, () => '.'.repeat(13)), unknown: 0 } };
  scan.coverage.rows[6] = '....#####....';
  // 模拟现有寻路器返回整段节点，首个默认起点的一条边不可达。
  const path = (a, b, allowed) => a.x === -3 ? null : [a, b].filter(allowed);
  const found = validateRoute(scan, undefined, undefined, path);
  assert.equal(found.status, 'validated');
  assert.equal(found.waypoints[0].x, -4);
  assert.deepEqual(found.lengths, [2, 2, 2, 2]);
  assert.equal(validateRoute(scan, 'unrelated', found.waypoints, path).reasonCode, 'wall_conflicts_with_pointing');
  assert.equal(validateRoute(scan, 'wall', [found.waypoints[0], found.waypoints[2], found.waypoints[1], found.waypoints[3]], path)
    .reasonCode, 'checkpoints_do_not_surround_wall');
  assert.equal(validateRoute(scan, 'wall', found.waypoints.map(p => ({ ...p, z: -3 })), path)
    .reasonCode, 'checkpoints_do_not_surround_wall');
  assert.equal(validateRoute(scan, 'wall', found.waypoints, (a, b) => [a, { x: 0, y: -60, z: 0 }, b])
    .reasonCode, 'route_not_passable');
  scan.coverage.rows[4] = '..?..........';
  assert.equal(validateRoute(scan, 'wall', found.waypoints, path).reasonCode, 'route_coverage_required');
  scan.coverage.rows[4] = '..x..........';
  assert.equal(validateRoute(scan, 'wall', found.waypoints, path).reasonCode, 'waypoint_not_standable');
});

// 引导和半圈不能计为完成；每圈实际回到起点后才累加。
test('route records each actual checkpoint without counting a partial lap', async () => {
  const bounds = { minX: 0, maxX: 3, minZ: 0, maxZ: 0, y: -60 };
  const waypoints = [[-1, -1], [4, -1], [4, 1], [-1, 1]].map(([x, z]) => ({ x, y: -60, z }));
  const route = { id: 'checked', bounds, waypoints }, result = {};
  let calls = 0;
  const movement = { check() {}, async move(point, allowed) {
    assert.equal(Boolean(allowed), calls > 0);
    if (allowed) assert.ok(allowed(point));
    assert.equal(point, waypoints[calls++ % 4]);
    return { ...point, x: point.x + 0.5, z: point.z + 0.5 };
  } };
  await runRoute(route, 2, result, movement);
  assert.equal(calls, 9);
  assert.deepEqual(result.route.checkpoints.map(({ index }) => index), [0, 1, 2, 3, 4, 5, 6, 7, 8]);
  assert.deepEqual(result.progress, { phase: 'route', checkpoint: 8, laps: 2, requested: 2 });

  const partial = {};
  await assert.rejects(runRoute(route, 2, partial, { ...movement, async move(point) {
    return point === waypoints[2] ? { ...point, x: point.x + 1 } : point;
  } }), { code: 'route_checkpoint_unverified' });
  assert.equal(partial.route.laps, 0);
  assert.deepEqual(partial.route.checkpoints.map(({ index }) => index), [0, 1]);
});

// 原话视锥可见偏离准星的原木，遮挡后的方块最多出现在附近来源中。
test('object focus merges ray, cone and nearby sources without retargeting the old view', async () => {
  const { bot, player, context } = fixture(), result = {};
  const original = new Position(0.5, -60, -2.5);
  bot.entity.position = new Position(1.5, -60, 1.5);
  player.position = new Position(1.5, -60, 1.5);
  context.messageAnchor = () => ({ center: original, anchor: { position: original, eye: new Position(0.5, -58.5, -2.5),
    facing: { yaw: Math.PI, pitch: 0 } } });
  const map = new Map([['0,-59,1', 'stone'], ['1,-59,1', 'oak_log'], ['0,-58,1', 'dirt'], ['0,-59,2', 'emerald_block']]);
  bot.blockAt = ({ x, y, z }) => ({ name: map.get(`${x},${y},${z}`) ?? 'air' });
  let frame = 0;
  context.check = () => { if (++frame === 2) player.position = new Position(-5.5, -60, 0.5); };
  await observeWindow(bot, { sources: ['message', 'player', 'self'], message: 12, focus: 'objects', radius: 1,
    samples: { count: 2, intervalMs: 100 } }, result, context);
  const [a, b] = result.observation.samples.map(sample => sample.views[0].playerFocus);
  assert.deepEqual(a.hit, { x: 0, y: -59, z: 1, block: 'stone' });
  const log = a.objects.find(object => object.block === 'oak_log');
  assert.deepEqual(log.sources, ['cone', 'hit_neighbor', 'player_nearby', 'self_nearby']);
  assert.equal(log.visible, true);
  const dirt = a.objects.find(object => object.block === 'dirt');
  assert.equal(dirt.visible, true);
  assert.equal(dirt.below, 'stone');
  const occluded = a.objects.find(object => object.block === 'emerald_block');
  assert.deepEqual(occluded.sources, ['player_nearby', 'self_nearby']);
  assert.ok(!occluded.visible);
  assert.equal(new Set(a.objects.map(object => object.id)).size, a.objects.length);
  assert.deepEqual(a.hit, b.hit);
  assert.deepEqual(b.objects.find(object => object.id === log.id).sources, ['cone', 'hit_neighbor', 'self_nearby']);
});

// 参照在等待走近时变化，执行器不能把旧观察当成当前游戏状态。
test('dig target requires the same loaded support immediately before breaking', () => {
  const { bot } = fixture();
  const point = new Position(0, -58, 1), reference = { x: 0, y: -59, z: 1, block: 'stone' };
  let support = { name: 'stone' };
  bot.blockAt = () => support;
  assert.ok(matchesDigReference(bot, point, reference));
  assert.ok(!matchesDigReference(bot, point.offset(1, 0, 0), reference));
  support = { name: 'air' };
  assert.ok(!matchesDigReference(bot, point, reference));
  support = null;
  assert.ok(!matchesDigReference(bot, point, reference));
});

// 回放协议量化后的准星擦过邻格边缘，直接命中应保留最先经过的实际方块。
test('direct focus keeps the first block even for a short crossing at an edge', () => {
  const { bot } = fixture();
  const center = new Position(0.165771484375, -60, -0.5);
  bot.blockAt = ({ y }) => ({ name: y < -60 ? 'grass_block' : 'air' });
  const focus = playerFocus(bot, { eye: center.offset(0, 1.62, 0),
    facing: { yaw: 4.319689898685965, pitch: -0.6872233929727671 } }, center, 'hit');
  assert.deepEqual(focus.hit, { x: 1, y: -61, z: 0, block: 'grass_block' });
});
