const assert = require('node:assert/strict');
const { test } = require('node:test');
const { blockSurvey, observedDoors } = require('../../plugins/mc/bridge/block-survey.cjs');
const { playerFocus } = require('../../plugins/mc/bridge/player-focus.cjs');

// 用最少的坐标运算模拟真实客户端给出的观测中心和样本格。
class Position {
  // 保存游戏坐标。
  constructor(x, y, z) { Object.assign(this, { x, y, z }); }
  // 使用方块整格保留负坐标方向。
  floored() { return new Position(Math.floor(this.x), Math.floor(this.y), Math.floor(this.z)); }
  // 按原格在三个维度上偏移。
  offset(x, y, z) { return new Position(this.x + x, this.y + y, this.z + z); }
  // 返回独立的格点坐标，供视线读取复用坐标类型。
  clone() { return new Position(this.x, this.y, this.z); }
  // 将当前坐标修改为要读取的格点。
  set(x, y, z) { this.x = x; this.y = y; this.z = z; return this; }
  // 测量扫描中心和角色之间的距离。
  distanceTo(other) { return Math.hypot(this.x - other.x, this.y - other.y, this.z - other.z); }
}

// 三个坐标方向各有一个独立的样本，并保留墙上的空缺和未知格。
test('the survey projects the existing wall and its gap against an exact origin', () => {
  const center = new Position(-1.2, -60, 0.5);
  const values = new Map([['-2,-60,0', 'dirt'], ['-2,-59,0', 'oak_planks'], ['-1,-60,1', 'cobblestone']]);
  const bot = { entity: { position: center },
    // 交付三个已加载的墙体格，剩下的格子保留为空或未加载。
    blockAt({ x, y, z }) { return x === 0 && z === 0 ? null : { name: values.get(`${x},${y},${z}`) ?? 'air' }; },
  };
  const survey = blockSurvey(bot, center);
  assert.deepEqual(survey.origin, new Position(-7, -61, -5));
  // 把绝对坐标映射回行内位置进行对照。
  const cell = (x, y, z) => {
    const glyph = survey.layers[y + 61][z + 5][x + 7];
    return survey.palette[glyph] ?? glyph;
  };
  assert.equal(cell(-2, -60, 0), 'dirt');
  assert.equal(cell(-2, -59, 0), 'oak_planks');
  assert.equal(cell(-1, -60, 1), 'cobblestone');
  assert.equal(cell(-1, -60, 0), '.');
  assert.equal(cell(0, -60, 0), '?');
  assert.throws(() => blockSurvey(bot, new Position(40, -60, 0)), /范围/);
});

// 用实机旧门的两格坐标验证合并，并区分只看到一半和另一半不可读取。
test('the two observed halves are one door at the lower cell', () => {
  const lower = new Position(7, -60, -3);
  const upper = lower.offset(0, 1, 0);
  // 按实际方块属性提供朝向、合页和所在半格。
  const door = (position, half, facing = 'south') => ({
    name: 'oak_door', position, getProperties: () => ({ half, facing, hinge: 'left' }),
  });
  const halves = [door(lower, 'lower'), door(upper, 'upper')];
  const bot = {
    // 对照已加载的方块，缺失表示不可验证。
    blockAt: pos => halves.find(b => b.position.distanceTo(pos) === 0),
  };
  const complete = [{ id: '7,-60,-3', block: 'oak_door', status: 'complete', lower, upper }];
  assert.deepEqual(observedDoors(bot, [halves[1], halves[0]]), complete);
  assert.deepEqual(observedDoors(bot, [halves[1]]), complete);
  halves.shift();
  const incomplete = [{ id: '7,-60,-3', block: 'oak_door', status: 'incomplete', upper }];
  assert.deepEqual(observedDoors(bot, halves), incomplete);
  halves.push(door(lower, 'lower', 'north'));
  assert.deepEqual(observedDoors(bot, [halves[0]]), incomplete);
});

// 两个可能的开口只在已知视线穿过一处时给出默认候选。
test('original sight distinguishes a wall gap, a doorway and unavailable samples', () => {
  const center = new Position(3.5, -60, -0.5);
  const solid = new Set(['2,-59,-3', '3,-60,-3', '4,-59,-3', '6,-60,-3', '8,-60,-3', '6,-59,-3', '8,-59,-3', '7,-61,-3']);
  let occluded = false;
  const bot = { entity: { position: new Position(7.5, -60, -1.25) },
    // 用固定已加载格模拟当场墙体，并可令原视线中途遇到未知格。
    blockAt({ x, y, z }) {
      if (occluded && x === 3 && y === -59 && z === -2) return null;
      return solid.has(`${x},${y},${z}`) ? { name: 'dirt', boundingBox: 'block' } : { name: 'air', boundingBox: 'empty' };
    },
  };
  const eye = { x: center.x, y: -58.38, z: center.z };
  const facing = { yaw: 0, pitch: -0.05 };
  const point = playerFocus(bot, { eye, facing }, center);
  assert.equal(point.choice, '3,-59,-3:x');
  assert.ok(point.candidates.some(gap => gap.id === '7,-60,-3:x' && gap.height === 2));
  assert.equal(playerFocus(bot, { eye, facing: { yaw: Math.PI, pitch: -0.05 } }, center).status, 'ambiguous');
  assert.equal(playerFocus(bot, { facing }, center).choice, null);
  occluded = true;
  assert.equal(playerFocus(bot, { eye, facing }, center).status, 'unavailable');
});

// 原话命中宽开口的第二列，移动到门洞旁的角色仍能识别原开口。
test('a wide wall gap has one identity distinct from the narrow doorway', () => {
  const center = new Position(3.5, -60, -2.5);
  const eye = center.offset(0, 1.62, 0);
  const facing = { yaw: 0, pitch: -0.36 };
  let supportLoaded = true;
  const bot = {
    entity: { position: new Position(7.5, -60, -4.5) },
    // 模拟两格宽开口、分开的门洞与其中一个未知支撑格。
    blockAt({ x, y, z }) {
      if (x === 3 && y === -61 && z === -5 && !supportLoaded) return null;
      const side = x >= 0 && x <= 8 && ![2, 3, 7].includes(x);
      const filled = z === -5 && (y === -61 || (side && (y === -60 || y === -59)));
      return filled ? { name: 'dirt', boundingBox: 'block' } : { name: 'air', boundingBox: 'empty' };
    },
  };
  const focus = playerFocus(bot, { eye, facing }, center);
  assert.equal(focus.choice, '2,-60,-5:x');
  assert.deepEqual(focus.candidates.map(({ id, width, height, pointed }) => [id, width, height, pointed]), [
    ['2,-60,-5:x', 2, 2, true], ['7,-60,-5:x', 1, 2, false]
  ]);
  supportLoaded = false;
  assert.ok(!playerFocus(bot, { eye, facing }, center).candidates.some(gap => gap.width === 2));
});
