const { setTimeout: delay } = require('node:timers/promises');
const { isDrop, dropStack, readDropTarget, validDropReference } = require('./dropped-items.cjs');
const { trackDrop } = require('./drop-tracker.cjs');
const { inventory, inventoryCapacity, inventoryDelta } = require('./inventory-capacity.cjs');
const { pickupStand } = require('./collection.cjs');

// 为独立拾取保留宿主与玩家可以区分的受阻原因。
function blocked(code, message) { return Object.assign(new Error(message), { code }); }

// 给步行拾取留出角色宽度和移向格中心的余量，检查这个脚位附近是否还有其他已知掉落。
function nearbyStack(others, feet) {
  return others.find(({ position: p }) => Math.abs(p.x-feet.x) <= 1.8 && Math.abs(p.z-feet.z) <= 1.8 &&
    p.y >= feet.y-1 && p.y <= feet.y+2.5);
}

// 从已退出且原因明确的原动作计算可续取余量，归属缺证、取消和完成均不产生新动作。
// 步骤：核对原号、退出与对象 → 对照收取包和库存 → 计算同一原堆余量。
function pickupRemainder(previous, source, args) {
  const r = previous?.receipt, p = r?.result.pickup, target = r?.result.dropTarget;
  const observed = p?.observed;
  const recoverable = ['pickup_other_stack_nearby','pickup_insufficient_space','pickup_capacity_unknown','pickup_path_blocked',
    'pickup_stand_unavailable','pickup_outside_observation','pickup_timed_out','pickup_other_collector'];
  if (!r?.exited) throw blocked('pickup_exit_unconfirmed','原拾取是否退出尚未核实，请先查询原号');
  if (r.result.status === 'done') throw blocked('pickup_already_completed','原堆已经完成拾取，请查原号');
  if (r.result.status === 'interrupted') throw blocked('pickup_original_interrupted','原工作已中断，请保留已产生的效果');
  if (r.skill !== 'pickup_drop' || r.result.status !== 'blocked' || !recoverable.includes(r.result.reasonCode) ||
      previous.id !== args.resumeWork || r.generation !== source.generation || target?.dimension !== source.dimension ||
      target.observation !== source.id || target.entity !== args.entity || p?.item !== args.item.replace(/^minecraft:/,'') ||
      p.stockCurrent !== true || !Number.isSafeInteger(p.verifiedCount) || p.verifiedCount !== (observed?.selfCount ?? 0) ||
      (p.approachAttempted && observed?.uuid !== args.entity))
    throw blocked('pickup_remainder_unconfirmed','需要已退出原工作的同一实体、可核实的收取包及入包读数');
  const prior = p.continuation;
  const originalCount = target.original.count, verifiedBefore = (prior?.verifiedBefore ?? 0)+p.verifiedCount;
  const otherBefore = (prior?.otherBefore ?? 0)+(observed?.otherCount ?? 0);
  if (!Number.isSafeInteger(otherBefore) || otherBefore < 0 || verifiedBefore < 0 || args.count < 1 ||
      originalCount-verifiedBefore-otherBefore !== args.count)
    throw blocked('pickup_remainder_mismatch','续取数量必须等于原整堆扣除已核实收取后的剩余量');
  return {work:previous.id,root:prior?.root ?? previous.id,sourceMessage:prior?.sourceMessage ?? r.order,
    originalCount,verifiedBefore,otherBefore};
}

// 走近原观察确认的整堆物品，按目标实体的拾取包和本次背包变化分别结算。
// 步骤：复核原栈、现场身份和容量 → 在原观察范围内接近目标 → 读回拾取人与库存 → 等待导航退出。
async function pickupDrop(bot, args, source, check, result, movement, continuation, claim) {
  if (!validDropReference(args) || typeof args.item !== 'string' || !/^(?:minecraft:)?[a-z0-9_]{1,80}$/.test(args.item) ||
      !Number.isSafeInteger(args.count) || args.count < 1 || args.count > 0x7fffffff)
    throw blocked('pickup_parameters', '拾取需要原观察、实体 UUID、物品和整堆数量');
  const item = args.item.replace(/^minecraft:/, '');
  const target = result.dropTarget = readDropTarget(bot, source, args.entity);
  const actor = bot.entity, dimension = bot.game.dimension;
  const before = inventory(bot);
  const entry = { state: 'unseen' };
  const pickup = result.pickup = { item, requested: args.count, stockBefore: before[item] ?? 0, approachAttempted: false,
    observed: entry,...(continuation && {continuation}) };
  const now = movement.now ?? (() => performance.now()), pause = movement.pause ?? delay;
  const start = now();
  let tracker, walk, walking = false, walkError, lastStand, confirmationSince, phase = 'checking_target';
  const inside = point => source.position.distanceTo(point) <= source.radius;
  // 按当前包和库存发布同一工作最近的进展，期间收到的其他物品单独保留。
  function sample(next = phase) {
    phase = next;
    const sampledAtMs = Math.floor(now());
    pickup.stockCurrent = bot.entity === actor && bot.game.dimension === dimension;
    if (pickup.stockCurrent) {
      const after = inventory(bot);
      pickup.stockAfter = after[item] ?? 0;
      pickup.stockSampledAtMs = sampledAtMs;
      pickup.verifiedCount = Math.min(entry.selfCount ?? 0, Math.max(0, pickup.stockAfter-pickup.stockBefore));
      result.inventoryDelta = inventoryDelta(before, after);
    }
    result.progress = { phase, sampledAtMs, entity: args.entity, item, requested: args.count,
      selfCount: entry.selfCount ?? 0, otherCount: entry.otherCount ?? 0, stock: pickup.stockAfter };
  }
  try {
    sample('checking_target');
    movement.setSampler?.(sample);
    if (target.original.item !== item || target.original.count !== (continuation?.originalCount ?? args.count))
      throw blocked('pickup_whole_stack_required', '自然拾取会接收整堆，请核对原观察的物品和数量');
    if (target.state !== 'tracked') throw blocked('pickup_target_unavailable', '原堆已失去追踪或缺少当前栈读数');
    if (target.item !== item || target.count !== args.count) throw blocked('pickup_stack_changed', '原堆物品或数量已经改变');
    claim?.();
    if (!target.withinOriginalRadius || !inside(bot.entity.position))
      throw blocked('pickup_outside_observation', '角色或目标已移出原观察半径');
    const capacity = result.capacity = inventoryCapacity(bot, item);
    if (capacity.status === 'unavailable' || capacity.knownSpace < args.count) {
      if (capacity.status === 'sampled') throw blocked('pickup_insufficient_space', '主背包和快捷栏的容量不足以接收整堆');
      throw blocked('pickup_capacity_unknown', '已读槽位的容量尚不足以确认能接收整堆');
    }

    const selected = bot.entities[target.entityId];
    tracker = trackDrop(bot, entry, (entity, stack) => entity === selected && entity.uuid === args.entity && stack.item === item, undefined, now);
    let others = [];
    while (true) {
      check();
      const entity = tracker.live();
      sample(walking ? 'approaching' : 'awaiting_pickup');
      if (!pickup.stockCurrent) throw blocked('pickup_world_changed', '原角色或维度已改变，保留末次库存读回');
      if (entry.otherCount > 0) throw blocked('pickup_other_collector', '其他实体已拾取原堆中的物品');
      if (entry.selfCount > args.count) throw blocked('pickup_quantity_changed', '服务端实际收取量超过原观察数量');
      if (entry.selfCount === args.count && pickup.verifiedCount === args.count) break;
      if (entry.selfCount === args.count) {
        sample('waiting_inventory');
        confirmationSince ??= now();
        if (now()-confirmationSince >= 800) throw blocked('pickup_stock_unconfirmed', '已收到拾取包，库存增量仍未确认');
      } else {
        if (!entity) throw blocked('pickup_lost', '原堆已不在视野内，剩余数量的归属未知');
        const stack = dropStack(entity);
        if (!stack || stack.item !== item || (stack.count !== args.count && stack.count !== args.count-entry.selfCount))
          throw blocked('pickup_stack_changed', '原堆物品或数量已经改变');
        if (!entity.position || !bot.entity?.position || !inside(entity.position) || !inside(bot.entity.position))
          throw blocked('pickup_outside_observation', '角色或目标已移出原观察半径');
        if (walkError) throw blocked('pickup_path_blocked', '本次未能抵达拾取站位');
        const stand = pickupStand(bot, entity.position.floored());
        if (!stand) throw blocked('pickup_stand_unavailable', '原堆附近没有已知且可站立的位置');
        others = Object.values(bot.entities).filter(other => other !== entity && isDrop(other) && other.position &&
          source.position.distanceTo(other.position) <= source.radius+4);
        const blocker = nearbyStack(others, stand.offset(0.5, 0, 0.5)) ?? nearbyStack(others, bot.entity.position);
        if (blocker) {
          result.nearbyDrop = { entityId: blocker.id, ...(blocker.uuid && { uuid: blocker.uuid }), stack: dropStack(blocker),
            position: blocker.position.clone(), sampledAtMs: Math.floor(now()) };
          throw blocked('pickup_other_stack_nearby', '另一堆地面物品靠近拾取站位或当前脚位，继续移动可能一起捡起');
        }
        if (!walking && !stand.equals(bot.entity.position.floored()) && (!lastStand || !stand.equals(lastStand))) {
          lastStand = stand;
          pickup.approachAttempted = walking = true;
          walk = movement.go(stand, point => {
            const feet = point.offset(0.5, 0, 0.5);
            return inside(feet) && !nearbyStack(others, feet);
          })
            .catch(error => { walkError = error; }).finally(() => { walking = false; });
        }
      }
      if (now()-start >= 8000) throw blocked('pickup_timed_out', '本次等待已到期限，拾取未完成');
      await pause(50);
    }
    sample('received');
  } finally {
    try {
      if (walking) movement.stop();
      if (walk) await walk;
    } finally {
      try { tracker?.stop(); sample(); }
      finally { movement.setSampler?.(undefined); }
    }
  }
  if (!pickup.stockCurrent || pickup.verifiedCount !== args.count || entry.selfCount !== args.count || entry.otherCount !== 0)
    throw blocked('pickup_stock_unconfirmed', '退出时未能同时核实原堆收取量与库存增量');
}

module.exports = { pickupDrop, pickupRemainder };
