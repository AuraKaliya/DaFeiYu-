const uuid = /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/;

// 对照同一工作窗口的服务端丢物和实体入场，再按数量守恒核验收件与合堆。
// 步骤：绑定同角色同材质的投物 → 追踪纯来源的堆与实际库存读回 → 汇总已知归属和未知余量。
function handoffEvidence(rows, mark, actor, recipient, item, submitted, sourceThrough = Infinity) {
  const material = 'minecraft:' + item;
  const kind = row => row.kind.startsWith('item.') ? row.kind.slice(5) : row.kind;
  const count = n => Number.isSafeInteger(n) && n >= 0;
  if (!uuid.test(mark.stream) || !count(mark.seq) || !uuid.test(actor) || !uuid.test(recipient) || !count(submitted) || submitted > 2304)
    throw new Error('无效的交物取证范围');
  const proof = { source: 'server_events', stream: mark.stream, afterSeq: mark.seq, throughSeq: mark.seq,
    thrown: 0, recipientPicked: 0, otherPicked: 0, throwUnknown: submitted, pickupUnknown: 0, stacks: [], omittedStacks: 0 };
  const added = new Map(), after = new Map(), origins = new Map();
  for (const row of rows) {
    if (row.stream !== mark.stream || !Number.isSafeInteger(row.seq) || row.seq <= proof.throughSeq)
      throw new Error('交物观察发生换流或乱序');
    proof.throughSeq = row.seq;
    if (kind(row) === 'added' && row.material === material) added.set(row.item,added.has(row.item) ? null : row);
    if (['pickupReadback','mergeReadback'].includes(kind(row))) after.set(row.cause,after.has(row.cause) ? null : row);
  }
  for (const row of rows) {
    if (row.seq > sourceThrough || kind(row) !== 'playerDrop' || row.actor !== actor || row.material !== material) continue;
    const spawn = added.get(row.item);
    if (!uuid.test(row.item) || !count(row.amount) || !row.amount || !spawn || !Number.isInteger(row.networkId) ||
        row.networkId < -2147483648 || row.networkId > 2147483647 || spawn.networkId !== row.networkId || spawn.amount !== row.amount) continue;
    if (origins.has(row.item) || proof.thrown+row.amount > submitted) throw new Error('交物窗口含有无法分开的投物');
    proof.thrown += row.amount;
    origins.set(row.item,{ readySeq: Math.max(row.seq,spawn.seq), amount: row.amount });
    if (proof.stacks.length < 12) proof.stacks.push({ uuid: row.item, count: row.amount, dropSeq: row.seq, spawnSeq: spawn.seq });
    else ++proof.omittedStacks;
  }
  const ground = new Map(), following = new Map(), byActor = new Map(), collected = new Map();
  for (let i = rows.length-1; i >= 0; --i) {
    const row = rows[i];
    if (kind(row) !== 'pickup' || row.material !== material) continue;
    const next = byActor.get(row.actor);
    if (next && next.tick === row.tick) following.set(row.seq,next);
    byActor.set(row.actor,row);
  }
  for (let i = 0; i < rows.length; ++i) {
    const r = rows[i], type = kind(r);
    if (r.material !== material) continue;
    const origin = origins.get(r.item);
    if (origin?.readySeq === r.seq) ground.set(r.item,origin.amount);
    if (type === 'merge' && (ground.has(r.item) || ground.has(r.target))) {
      const settled = after.get(r.seq);
      const pure = ground.get(r.item) === r.amount && ground.get(r.target) === r.targetAmount;
      ground.delete(r.item);
      ground.delete(r.target);
      if (pure && settled && settled.seq > r.seq && kind(settled) === 'mergeReadback' && count(settled.sourceRemaining) && count(settled.targetRemaining) &&
          settled.sourceRemaining <= r.amount && settled.targetRemaining >= r.targetAmount &&
          settled.sourceRemaining+settled.targetRemaining === r.amount+r.targetAmount) {
        if (settled.sourceRemaining) ground.set(r.item,settled.sourceRemaining);
        if (settled.targetRemaining) ground.set(r.target,settled.targetRemaining);
      }
    }
    if (type !== 'pickup' || !ground.has(r.item)) continue;
    const held = ground.get(r.item), taken = held-r.remaining;
    ground.delete(r.item);
    const end = after.get(r.seq);
    if (!end || kind(end) !== 'pickupReadback' || end.seq <= r.seq || end.actor !== r.actor || !uuid.test(r.actor) || !count(r.remaining) ||
        taken <= 0 || (r.amount !== held && r.amount !== taken) || !count(r.beforeStock) || end.ground !== r.remaining) continue;
    // 同一刻内可接连拾取几堆。下一收取的事前实存是上一收取的独立库存读数。
    const next = following.get(r.seq);
    const stock = next && next.seq < end.seq ? next.beforeStock : end.afterStock;
    if (!count(stock) || stock-r.beforeStock !== taken) continue;
    proof[r.actor === recipient ? 'recipientPicked' : 'otherPicked'] += taken;
    collected.set(r.actor,(collected.get(r.actor) ?? 0)+taken);
    if (r.remaining) ground.set(r.item,r.remaining);
  }
  proof.throwUnknown = submitted-proof.thrown;
  proof.pickupUnknown = proof.thrown-proof.recipientPicked-proof.otherPicked;
  proof.collectors = [];
  if (collected.has(recipient)) {
    proof.collectors.push({ uuid: recipient, count: collected.get(recipient) });
    collected.delete(recipient);
  }
  proof.omittedOtherUnits = 0;
  for (const [uuid,units] of collected) {
    if (proof.collectors.length < 9) proof.collectors.push({ uuid, count: units });
    else proof.omittedOtherUnits += units;
  }
  return proof;
}

module.exports = { handoffEvidence };
