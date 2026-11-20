const assert = require('node:assert/strict');
const { test } = require('node:test');
const { blockSurvey } = require('../../plugins/mc/bridge/block-survey.cjs');

// 用最少的坐标运算模拟真实客户端给出的观测中心和样本格。
class Position {
  // 保存游戏坐标。
  constructor(x, y, z) { Object.assign(this, { x, y, z }); }
  // 使用方块整格保留负坐标方向。
  floored() { return new Position(Math.floor(this.x), Math.floor(this.y), Math.floor(this.z)); }
  // 按原格在三个维度上偏移。
  offset(x, y, z) { return new Position(this.x + x, this.y + y, this.z + z); }
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
