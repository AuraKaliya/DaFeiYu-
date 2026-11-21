const assert = require('node:assert/strict');
const { test } = require('node:test');
const { observationInput, observeWindow } = require('../../plugins/mc/bridge/partner-observe.cjs');

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
    messageAnchor: () => ({ anchor: { position: origin }, center: origin }), check: () => {} };
  return { bot, player, context, reads };
}

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

// 玩家移动后，原话中心保持不动，变化只比较前后都可读的格子。
test('sample series keeps original message apart from live player', async () => {
  const { bot, player, context } = fixture(), result = {};
  let frame = 0;
  context.check = () => { if (++frame === 2) player.position = player.position.offset(1, 0, 0); };
  await observeWindow(bot, { sources: ['message', 'player'], message: 12, radius: 1, samples: { count: 2, intervalMs: 100 } }, result, context);
  const [first, second] = result.observation.samples;
  assert.ok(second.sampledAtMs - first.sampledAtMs >= 90);
  assert.equal(second.views[0].message, 12);
  assert.equal(second.views[0].anchor.position.x, 0);
  assert.equal(second.views[1].actor.position.x, 2);
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
  await assert.rejects(observeWindow(bot, { sources: ['message'] }, {}, context), { code: 'observe_invalid_sources' });
  assert.equal(reads.size, 0);
});
