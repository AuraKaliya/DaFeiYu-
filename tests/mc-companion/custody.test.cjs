const test = require('node:test');
const assert = require('node:assert/strict');
const { carriedAt } = require('../../plugins/mc/bridge/returning.cjs');
const destination = { x: 0, y: -60, z: 0 };
const position = { x: 0.5, y: -60, z: 0.5 };
const departure = { stream: 'test-stream', seq: 40 };
const records = Array.from({ length: 16 }, (_, n) => ({
  pickup: { stream: 'test-stream', first: 2 * n + 1, last: 2 * n + 2, lower: 1, upper: 1 },
}));
// 从同一位置、角色及采样序号组成一份固定联合读数。
function at(seq, stock) { return { kind: 'positionStock', actor: 'fish', stream: 'test-stream', seq, stock, ...position }; }
// 按当前客户端读数核对服务端样本。
function read(rows, stock, observations = records) {
  return carriedAt(rows, observations, 'fish', destination, departure, { position, cobblestone: stock });
}
test('未到、旧样本、错角色和换流均不能证明交付', () => {
  for (const wrong of [at(39, 21), { ...at(44, 21), x: 11 }, { ...at(44, 21), actor: 'other' }, { ...at(44, 21), stream: 'other' }])
    assert.equal(read([wrong], 21), null);
  assert.equal(read([at(44, 21)], 22), null);
  assert.equal(read([at(44, 21)], 21, [...records.slice(1), records[1]]), null);
});
test('实际持有保持固定数量、明确不足及混合丢失区间', () => {
  const debit = { kind: 'stoneDebit', actor: 'fish', seq: 41, amount: 2 };
  const full = read([at(44, 21)], 21);
  assert.deepEqual([full.lower, full.upper], [16, 16]);
  const short = read([debit, at(44, 14)], 14);
  assert.deepEqual([short.lower, short.upper], [14, 14]);
  const mixed = read([debit, at(44, 19)], 19);
  assert.deepEqual([mixed.lower, mixed.upper], [14, 16]);
});
