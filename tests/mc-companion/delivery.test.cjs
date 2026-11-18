const assert = require('node:assert/strict');
const test = require('node:test');
const { collectionDesk } = require('../../plugins/mc/bridge/delivery.cjs');

// 固定两个来源和真实问题语义，单独验证接单去重与只读查询。
function fixture() {
  let world = 'original';
  const question = { id: '3', generation: world, sent: true, owner: 'player', order: 2, after: 2,
    material: 'minecraft:cobblestone', returnPlace: 'riverside' };
  const inbox = { get: (_generation, number) => number === 3 ? { offer: { purpose: 'collect.return', question: '3', quantity: 16 } } : null };
  const desk = collectionDesk({ read: () => ({ generation: world }) }, inbox, () => question);
  const args = { generation: world, question: '3', grant: '5', owner: 'player', order: 2, quantity: 16,
    material: question.material, returnPlace: 'riverside', via: 'game', command: '3' };
  return { desk, args, changeWorld: () => { world = 'other'; } };
}

test('丢失接单回执后只读回查，重复提交保留原范围', () => {
  const { desk, args, changeWorld } = fixture();
  assert.equal(desk.read('original', '3', '5').phase, 'unreceived');
  const first = desk.admit(args);
  assert.equal(first.phase, 'accepted');
  assert.deepEqual(desk.read('original', '3', '5'), first);
  assert.deepEqual(desk.admit({ ...args }), first);
  assert.throws(() => desk.admit({ ...args, quantity: 12 }));
  assert.throws(() => desk.admit({ ...args, grant: '6' }));
  changeWorld();
  assert.equal(desk.read('original', '3', '5'), null);
  assert.throws(() => desk.admit(args));
});

test('缺失原话或目标冲突不能接单，受信管理答复保持自己的身份', () => {
  const { desk, args } = fixture();
  assert.throws(() => desk.admit({ ...args, command: '2' }));
  assert.throws(() => desk.admit({ ...args, owner: 'other' }));
  assert.throws(() => desk.admit({ ...args, returnPlace: 'elsewhere' }));
  assert.equal(desk.admit({ ...args, via: 'management', command: '1' }).via, 'management');
});
