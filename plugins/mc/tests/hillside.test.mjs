import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

const fixture = JSON.parse(readFileSync(new URL('./fixtures/hillside.json', import.meta.url), 'utf8'));

// 解释已归一化的固定采石样本，仅验证口径；真实游戏来源由后续双端探针采集。
// 步骤：区分查看与采集委托 → 按来源累计已确认材料与范围 → 对照到达时读数。
function evaluate(events, startingStock) {
  let phase = 'inspect';
  let stock = startingStock;
  let lower = 0;
  let upper = 0;
  let gap = false;
  const receipts = new Map();
  const transitions = {
    observed: ['inspect', 'waiting'],
    declined: ['waiting', 'declined'],
    authorized: ['waiting', 'authorized'],
    accepted: ['authorized', 'accepted'],
    started: ['accepted', 'mining'],
  };

  for (const [kind, identity, count] of events) {
    if (Object.hasOwn(transitions, kind)) {
      const [from, to] = transitions[kind];
      assert.equal(phase, from, kind);
      phase = to;
      continue;
    }
    assert.equal(phase, 'mining', kind);
    if (kind === 'gap') {
      gap = true;
      continue;
    }
    if (kind === 'arrived') {
      assert.equal(identity, fixture.meeting);
      if (!gap) assert.equal(count, stock, '到达时游戏库存与已知流量不符');
      stock = count;
      if (gap) phase = 'unverified';
      else if (lower !== upper) phase = 'uncertain';
      else phase = lower >= fixture.collect.quota ? 'delivered' : 'short';
      continue;
    }

    assert.ok(typeof identity === 'string' && identity.length > 0);
    assert.ok(Number.isSafeInteger(count) && count > 0);
    if (receipts.has(identity)) {
      assert.deepEqual(receipts.get(identity), [kind, count], '同一事件身份对应不同材料变化');
      continue;
    }
    receipts.set(identity, [kind, count]);
    switch (kind) {
      case 'self': stock += count; lower += count; upper += count; break;
      case 'gift': stock += count; break;
      case 'lostSelf':
        assert.ok(count <= lower);
        stock -= count; lower -= count; upper -= count;
        break;
      case 'lostMixed':
        stock -= count;
        lower = Math.max(0, lower - count);
        upper = Math.min(stock, upper);
        break;
      default: assert.fail('未知固定样本事件');
    }
    assert.ok(lower <= upper && upper <= stock);
  }
  return [phase, gap ? null : lower, gap ? null : upper, stock];
}

test('查看与采集是两项委托，发现、授权、接纳均不会冒充采掘', () => {
  assert.equal(fixture.version, 1);
  assert.equal(fixture.inspect.miningQuota, 0);
  assert.notEqual(fixture.collect.id, fixture.inspect.id);
  assert.equal(fixture.collect.parent, fixture.inspect.id);
  assert.equal(fixture.collect.quota, 16);
  assert.equal(fixture.material, 'minecraft:cobblestone');
  assert.deepEqual(evaluate([['observed']], 5), ['waiting', 0, 0, 5]);
  assert.deepEqual(evaluate([['observed'], ['authorized'], ['accepted']], 5), ['accepted', 0, 0, 5]);
  assert.throws(() => evaluate([['observed'], ['started']], 0));
});

for (const sample of fixture.cases) {
  test(sample.id, () => assert.deepEqual(evaluate(sample.events, sample.startingStock), sample.expected));
}
