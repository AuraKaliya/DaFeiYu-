const { isDeepStrictEqual } = require('node:util');

// 为单块双源夹具计算已被指定玩家拾取的自采数量范围。
// 步骤：按流序号去重 → 核对进入世界的两个来源 → 验证合堆与拾取读回 → 对各独立堆求范围。
function ownPickupRange(input, self, gift, collector) {
  if (!input.length || self.item === gift.item) return null;
  const unique = new Map();
  for (const row of input) {
    if (row.stream !== self.stream || row.world !== self.world) return null;
    if (unique.has(row.seq) && !isDeepStrictEqual(row, unique.get(row.seq))) return null;
    unique.set(row.seq, row);
  }
  const rows = [...unique.values()].sort((a, b) => a.seq - b.seq);
  if (rows.some((row, index) => row.seq !== rows[0].seq + index)) return null;
  const piles = new Map();
  for (const source of [self, gift]) {
    const origin = unique.get(source.seq);
    if (!isDeepStrictEqual(origin, source) || source.amount !== 1 ||
      !rows.some(row => row.kind === 'added' && row.item === source.item && row.networkId === source.networkId &&
        row.amount === 1)) return null;
    piles.set(source.item, { size: 1, own: Number(source.item === self.item), ground: 1, collected: 0 });
  }

  for (const row of rows) {
    const pile = piles.get(row.item);
    if (row.kind === 'merge' && (pile || piles.has(row.target))) {
      const target = piles.get(row.target);
      const after = rows.find(value => value.kind === 'mergeReadback' && value.cause === row.seq);
      // 这个夹具只解释两份已知且尚未分取的来源，混入第三份时不推测。
      if (!after || !target || !pile || pile === target || pile.ground !== 1 || target.ground !== 1 ||
        row.amount !== 1 || row.targetAmount !== 1 || after.sourceRemaining !== 0 || after.targetRemaining !== 2) return null;
      piles.delete(row.item);
      Object.assign(target, { size: 2, own: 1, ground: 2 });
    }
    if (row.kind === 'pickup' && pile) {
      const after = rows.find(value => value.kind === 'pickupReadback' && value.cause === row.seq);
      if (!after || after.actor !== row.actor || after.ground !== row.remaining) return null;
      const taken = pile.ground - after.ground;
      if (taken <= 0 || after.ground < 0 || after.afterStock - row.beforeStock !== taken) return null;
      pile.ground = after.ground;
      if (row.actor === collector) pile.collected += taken;
    }
  }

  let lower = 0;
  let upper = 0;
  for (const pile of piles.values()) {
    lower += Math.max(0, pile.collected - (pile.size - pile.own));
    upper += Math.min(pile.own, pile.collected);
  }
  return [lower, upper];
}

module.exports = { ownPickupRange };
