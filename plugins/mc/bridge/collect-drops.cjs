const { trackDrop } = require('./drop-tracker.cjs');

// 追踪一次开挖附近新出现的一堆物品，按协议拾取事件记录数量和拾取实体。
// 步骤：排除既有实体并绑定本次目标 → 追踪同一实体和拾取包 → 结束时保留末次状态并释放监听。
function trackBreakDrop(bot, point, wanted, result, now = () => performance.now()) {
  const known = new Set(Object.values(bot.entities));
  const center = point.offset(0.5, 0.5, 0.5);
  const evidence = result.pickups ??= { scope: 'one_new_stack_near_each_target', attempted: 0, omitted: 0,
    recent: [], selfCollected: {}, otherCollected: {} };
  const entry = { targetCell: { x: point.x, y: point.y, z: point.z }, state: 'unseen' };
  ++evidence.attempted;
  evidence.recent.push(entry);
  if (evidence.recent.length > 4) { evidence.recent.shift(); ++evidence.omitted; }
  return trackDrop(bot, entry, (entity, stack) => !known.has(entity) && center.distanceTo(entity.position) <= 3 &&
    (!wanted || stack.item === wanted), observed => {
    const { self, count } = observed.lastCollector;
    const totals = self ? evidence.selfCollected : evidence.otherCollected;
    totals[observed.item] = (totals[observed.item] ?? 0) + count;
  }, now);
}

// 从本次实测拾取、现存实体和库存缺口生成受阻原因。
function pickupShortfall(entry) {
  if (entry.selfCount > 0) return { code: 'pickup_stock_unconfirmed', message: '游戏已报告角色拾取，尚未读到所需物品净入包' };
  if (entry.otherCount > 0) return { code: 'pickup_other_collector', message: '游戏已报告其他实体拾取本次追踪物品，未核实角色取得所需数量' };
  if (entry.state === 'visible') return { code: 'pickup_still_visible', message: '本次追踪的掉落实体仍在视野内，未核实角色拾取' };
  if (entry.state === 'left_view') return { code: 'pickup_lost', message: '本次追踪的掉落已离开视野，拾取人未知' };
  return { code: 'pickup_unverified', message: '未读到目标附近新出现的所需物品，拾取未核实' };
}

module.exports = { trackBreakDrop, pickupShortfall };
