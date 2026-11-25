const assert = require('node:assert/strict');
const test = require('node:test');
const { exploration } = require('../bridge/exploration.cjs');

// 产生三次独立离开和重返，可选择每趟确实看到不同区域。
function returns(fresh) {
  const track = exploration(1);
  track.observe('0,0,0', 'old', 0);
  for (let trip = 1; trip <= 3; ++trip) {
    track.observe(trip + ',0,0', fresh ? 'wall-' + trip : 'old', trip * 100);
    track.observe('0,0,0', 'old', trip * 100 + 50);
  }
  return track;
}
if (require.main === module) {
  test('相同位置样本不构成三次返回，轨迹相同但可见覆盖有别', () => {
    const still = exploration(1);
    for (let i = 0; i < 16; ++i) still.observe('0,0,0', 'wall', i * 20);
    assert.equal(still.read(320), null);
    for (const fresh of [true, false]) {
      const track = returns(fresh);
      const fact = track.read(360);
      assert.deepEqual(fact.returns, [3, 5, 7]);
      assert.equal(fact.gained, fresh ? 3 : 0);
      assert.deepEqual(track.read(400), fact);
      assert.equal(track.read(1500), null);
      track.observe('0,0,0', 'new', 1600);
      assert.equal(track.read(1601), null);
    }
  });
}
module.exports = { returns };
