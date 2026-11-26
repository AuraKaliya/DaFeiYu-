const { Movements, goals } = require('mineflayer-pathfinder');
const { setTimeout: delay } = require('node:timers/promises');
const { tellOwner } = require('./chat.cjs');
const { blockSurvey, surveyCoverage, observedDoors } = require('./block-survey.cjs');
const { playerFocus } = require('./player-focus.cjs');
const { observationInput, observeWindow } = require('./partner-observe.cjs');
const { observeShore } = require('./shore.cjs');
const { observeDrops, readDropTarget } = require('./dropped-items.cjs');
const { trackBreakDrop, pickupShortfall } = require('./collect-drops.cjs');
const { pickupDrop } = require('./pickup-drop.cjs');
const { observeHandoff } = require('./handoff.cjs');
const { giveItem, refreshHandoff, handoffSample, originalHandoffReceipt } = require('./give-item.cjs');
const { chestWindows, chestSample, chestError } = require('./single-chest.cjs');
const { chestOrder, inventoryOrder, deliveryOrder, confirmedDelivery, transferItem, reconcileTransfer, chestProof } = require('./chest-transfer.cjs');
const { makeReceiptRoom } = require('./partner-records.cjs');
const { inventoryCapacity, inventory, inventoryDelta } = require('./inventory-capacity.cjs');
const { observeWall, surface, validateRoute, runRoute } = require('./partner-route.cjs');
const { placeBlock, placeTarget } = require('./placement.cjs');
const { craftPlanks } = require('./crafting.cjs');
const { followPlayer } = require('./follow.cjs');
const { visibleBlocks, fromGround, groundTypes, requiresCollectionApproval, affectedByDig, digAndReadback, collectionProgress, pickupStand,
  matchesDigReference } = require('./collection.cjs');
const { BuiltObjects } = require('./built-objects.cjs');
const { controllerLease } = require('./controller.cjs');
const { dryStand, clearStand, DryStandGoal, digReachable, DigStandGoal, PlayerMeetingGoal, monitorSurface } = require('./navigation.cjs');
const { monitorMove } = require('./move-progress.cjs');
const { belowFeet } = require('./world.cjs');

// 将宿主局部编号限定在原控制实例内。
function workKey(controller, id) { return controller + '/' + id; }

// 只在当前游戏注册表中解析原版方块名。
function blockType(bot, name) {
  if (typeof name !== 'string' || !/^(?:minecraft:)?[a-z0-9_]{1,80}$/.test(name)) throw new Error('需要游戏方块名');
  const block = bot.registry.blocksByName[name.replace(/^minecraft:/, '')];
  if (!block || block.name === 'air') throw new Error('当前版本没有可操作的这个方块');
  return block;
}

// 将三个整数解析为当前角色附近的世界坐标，指定目标时不接受缺轴或越界。
function targetPoint(bot, args) {
  if (![args.x, args.y, args.z].every(Number.isSafeInteger)) throw new Error('指定位置需要完整的整数 x、y、z');
  const point = bot.entity.position.clone().set(args.x, args.y, args.z);
  if (point.distanceTo(bot.entity.position) > 64) throw new Error('目标超过本次移动范围');
  return point;
}

// 从已加载的局部观察范围识别门，包括细碰撞盒导致视线检测漏掉的半格。
function localDoors(bot, type, center) {
  if (!center || center.distanceTo(bot.entity.position) > 24) return [];
  const origin = center.floored();
  const hits = bot.findBlocks({ matching: type.id, point: center, maxDistance: 9, count: 32 })
    .filter(pos => Math.abs(pos.x - origin.x) <= 5 && Math.abs(pos.z - origin.z) <= 5 &&
      pos.y >= origin.y - 4 && pos.y <= origin.y + 2)
    .map(pos => bot.blockAt(pos)).filter(block => block?.name === type.name);
  return observedDoors(bot, hits);
}

// 构造同一套只移动规则，读路线时保持正在运行的移动目标。
function walkingMovements(bot, allowed) {
  const moves = new Movements(bot);
  moves.canDig = moves.canOpenDoors = moves.allow1by1towers = moves.allowParkour = false;
  moves.scafoldingBlocks = [];
  if (allowed) moves.exclusionAreasStep.push(block => block.position && allowed(block.position) ? 0 : 101);
  return moves;
}

// 走到固定目标并等待寻路器真实退出。
function walk(bot, goal, allowed) {
  bot.pathfinder.setMovements(walkingMovements(bot, allowed));
  return bot.pathfinder.goto(goal);
}

// 用独立的寻路上下文检查目标，保留成功路径及求解器的终态。
function probePath(bot, from, goal, allowed) {
  const position = bot.entity.position.clone().set(from.x, from.y, from.z);
  let final;
  const results = bot.pathfinder.getPathFromTo(walkingMovements(bot, allowed), position, goal,
    { optimizePath: false, timeout: 100, tickTimeout: 25, searchRadius: 32 });
  for (const { result } of results) final = result;
  return final;
}

// 读取用于固定路线验证的原始节点。
function readPath(bot, from, to, allowed) {
  const result = probePath(bot, from, new goals.GoalBlock(to.x, to.y, to.z), allowed);
  return result?.status === 'success' ? result.path : null;
}

// 用当前可站立性和同一侧的约束走完一段，普通失败只重验一次。
// 步骤：核对目标 → 探测重规划条件 → 按原工作等待实际脚位。
async function walkRouteSegment(bot, target, allowed, check) {
  const point = bot.entity.position.clone().set(target.x, target.y, target.z);
  const traversable = allowed && (p => allowed(p) && surface(bot, p.clone().set(p.x, point.y, p.z)) === '.');
  const goal = new goals.GoalBlock(point.x, point.y, point.z);
  for (let attempt = 0; attempt < 2; ++attempt) {
    check();
    if (surface(bot, point) !== '.') break;
    if (goal.isEnd(bot.entity.position.floored())) return bot.entity.position.clone();
    if (attempt && !readPath(bot, bot.entity.position.floored(), target, traversable)?.length) break;
    const timer = setTimeout(() => { bot.pathfinder.setGoal(null); bot.clearControlStates(); }, 12000);
    try {
      await walk(bot, goal, traversable);
      check();
      return bot.entity.position.clone();
    } catch (error) {
      check();
      if (attempt) throw Object.assign(error, { code: 'route_segment_blocked' });
    } finally { clearTimeout(timer); }
  }
  throw Object.assign(new Error('当前站位或剩余路线已受阻'), { code: 'route_segment_blocked' });
}

// 以原工作号保留最近的现场证明或已核验路线。
function remember(records, id, value) {
  records.set(id, value);
  if (records.size > 16) records.delete(records.keys().next().value);
}

// 到实际掉落物脚下收取，回退到挖掘位置并限制占用前台的时间。
// 步骤：等待物品出现 → 核对支撑并导航 → 等待拾取更新或报告受阻。
async function pickup(bot, point, drops, check, result) {
  let expired = false;
  const timer = setTimeout(() => { expired = true; bot.pathfinder.setGoal(null); bot.clearControlStates(); }, 6000);
  try {
    await delay(400);
    for (let attempt = 0; attempt < 3; ++attempt) {
      check();
      if (expired) throw new Error('拾取超时');
      const tracked = drops.live();
      if (!tracked && drops.entry.state === 'left_view') return;
      const stand = pickupStand(bot, tracked?.position.floored() ?? point);
      if (!stand) throw new Error('掉落物附近没有已知站位');
      if (!stand.equals(bot.entity.position.floored())) await walk(bot, new goals.GoalBlock(stand.x, stand.y, stand.z));
      await delay(450);
      if (!drops.live()) return;
    }
    if (drops.live()) result.pickupIssue = '附近掉落物仍在客户端视野内';
  } catch (error) {
    check();
    result.pickupIssue = expired ? '未能在期限内抵达拾取站位' : String(error.message).slice(0, 120);
  } finally { clearTimeout(timer); }
}

// 为当前采掘阻碍保留可供后续决策使用的原因。
function digBlocked(code, message) {
  return Object.assign(new Error(message), { code });
}

// 逐块执行本次采集，指定坐标时保持目标，实际破坏与背包增减分别取证。
// 步骤：核对材料和目标 → 走近并选择可采工具 → 读回方块 → 走近掉落并记录背包变化。
async function collect(bot, args, check, job, built) {
  const result = job.receipt.result;
  const fixed = ['x', 'y', 'z'].some(k => Object.hasOwn(args, k)) ? targetPoint(bot, args) : null;
  if (!Number.isInteger(args.count) || args.count < 1 || args.count > 32 || (fixed && args.count !== 1))
    throw new Error('数量应为 1～32，指定的单格只能采一次');
  const referenceChanged = () => digBlocked('dig_reference_changed', '目标下方的原参照已变化');
  if (args.digReference && (!fixed || !matchesDigReference(bot, fixed, args.digReference))) throw referenceChanged();
  const before = inventory(bot);
  const expected = args.item?.replace(/^minecraft:/, '');
  const pickupItem = expected ?? args.pickupItem;
  if (Object.hasOwn(args, 'pickupItem') && (!fixed || expected || typeof args.pickupItem !== 'string' ||
      !Object.hasOwn(bot.registry.itemsByName, args.pickupItem)))
    throw digBlocked('pickup_item_invalid', '定点采掘需要可核实的拾取物品');
  if (Object.hasOwn(args, 'item') && (!Object.hasOwn(bot.registry.itemsByName, expected) ||
      args.requiredStock !== args.count || args.source !== 'ground'))
    throw new Error('地面取材需要已知物品和目标库存');
  const type = args.block === undefined && expected ? null : blockType(bot, args.block);
  const types = expected ? groundTypes(bot.registry, expected).filter(block => !type || block.id === type.id) : [type];
  const matching = types.map(block => block.id);
  const missing = expected ? Math.max(0, args.requiredStock - (before[expected] ?? 0)) : args.count;
  if (expected) result.stock = { item: expected, before: before[expected] ?? 0, required: args.requiredStock };
  result.broken = 0;
  result.changes = [];
  const approved = Boolean(fixed && args.source !== 'ground' && args.approvedBuiltEdit === true);
  const readProtected = ({ x, y, z }) => bot.blockAt(bot.entity.position.clone().set(x, y, z));
  const permitsDig = cells => built.permits(cells, approved, readProtected);
  const allowed = block => permitsDig(affectedByDig(bot, block)) &&
    (args.source !== 'ground' || fromGround(bot, block, args.protectedCells));
  // 拟动格与原确认范围重叠时，同一执行器拒绝任何未获批的采掘入口。
  const checkApproval = cells => {
    if (requiresCollectionApproval(bot, args.collectionGuards, cells))
      throw Object.assign(new Error('目标仍在需要重新确认的取材范围内'), { code: 'approval_required' });
  };
  const progress = collectionProgress(result, () => inventory(bot)[expected] ?? 0);
  job.sampleProgress = progress.sample;
  // 只有已读全的容量为零时停止取材，保留特殊物品栈和未同步槽位的不确定性。
  const checkCapacity = () => {
    if (!expected) return;
    const capacity = result.capacity = inventoryCapacity(bot, expected);
    if (capacity.status === 'sampled' && capacity.knownSpace === 0)
      throw digBlocked('pickup_inventory_full', '主背包和快捷栏已经放不下所需物品');
  };
  try {
    for (let n = 0; n < missing; ++n) {
      progress.enter('selecting');
      check();
      const stock = pickupItem ? (inventory(bot)[pickupItem] ?? 0) : 0;
      if (expected && stock >= args.requiredStock) break;
      checkCapacity();
      if (!matching.length) throw digBlocked('dig_source_missing', '当前地面没有这种物品的自然来源');
      const block = fixed ? bot.blockAt(fixed) : visibleBlocks(bot, matching, bot.entity.position, 1, allowed)[0];
      if (!block) throw fixed ? digBlocked('dig_target_unloaded', '目标格尚未加载') :
        digBlocked('dig_source_missing', '当前视野内没有符合取材范围的方块');
      if (!matching.includes(block.type)) {
        if (!fixed) throw digBlocked('dig_source_missing', '读取的候选已不是所需材质');
        result.targetBlock = { x: fixed.x, y: fixed.y, z: fixed.z, expected: type.name, observed: block.name, phase: 'initial' };
        throw digBlocked('dig_material_mismatch', '目标格读到' + block.name + '，请求材质是' + type.name);
      }
      progress.enter('checking_target', block);
      if (!block.diggable) throw digBlocked('dig_not_breakable', '该方块无法采掘');
      if (!permitsDig(affectedByDig(bot, block))) throw digBlocked('dig_protected', '目标属于已验收的对象或其支撑，需要指定拆改');
      if (!allowed(block)) throw digBlocked('dig_outside_scope', '目标不在地面取材范围内');
      checkApproval(affectedByDig(bot, block));
      const pos = block.position;
      if (pos.equals(belowFeet(bot.entity.position)))
        throw digBlocked('dig_stand_support', '目标支撑着角色，请先移动');
      if (!digReachable(bot, pos, bot.entity.position)) {
        progress.enter('approaching', block);
        try { await walk(bot, new DigStandGoal(bot, pos)); }
        catch (error) {
          check();
          throw digBlocked('dig_path_blocked', '无法走到目标的采掘站位：' + error.message);
        }
        finally { result.approachPosition = bot.entity.position.clone(); }
      }
      check();
      progress.enter('preparing', block);
      const suitable = item => !expected || !item?.enchants?.some(enchant => enchant.name?.endsWith('silk_touch'));
      const items = bot.inventory.items().filter(item => block.canHarvest(item.type) && suitable(item));
      const best = items.reduce((item, next) => !item || block.digTime(next.type) < block.digTime(item.type) ? next : item, null);
      if (best && (!suitable(bot.heldItem) || !block.canHarvest(bot.heldItem?.type) || block.digTime(best.type) < block.digTime(bot.heldItem?.type)))
        await bot.equip(best, 'hand');
      else if (!suitable(bot.heldItem)) await bot.unequip('hand');
      check();
      const here = bot.blockAt(pos);
      if (!here) throw digBlocked('dig_target_unloaded', '到达后目标格未加载');
      if (here.type !== block.type || !here.diggable) {
        if (fixed) result.targetBlock = { x: pos.x, y: pos.y, z: pos.z, expected: block.name, observed: here.name, phase: 'before_break' };
        throw digBlocked('dig_target_changed', '到达后目标方块已经变化');
      }
      if (!here.canHarvest(bot.heldItem?.type)) throw digBlocked('dig_tool_missing', '没有可采掘这个方块的工具');
      if (pos.equals(belowFeet(bot.entity.position))) throw digBlocked('dig_stand_support', '目标支撑着角色，请先移动');
      if (!bot.canDigBlock(here) || !digReachable(bot, pos, bot.entity.position))
        throw digBlocked('dig_stand_unusable', '实际站位没有安全的采掘视线');
      const affected = affectedByDig(bot, here);
      if (!permitsDig(affected)) throw digBlocked('dig_protected', '目标属于已验收的对象或其支撑，需要指定拆改');
      if (!allowed(here)) throw digBlocked('dig_outside_scope', '目标已不符合地面取材范围');
      if (!matchesDigReference(bot, pos, args.digReference)) throw referenceChanged();
      checkApproval(affected);
      if (expected && (inventory(bot)[expected] ?? 0) >= args.requiredStock) break;
      checkCapacity();
      progress.enter('digging', here);
      const drops = trackBreakDrop(bot, pos, pickupItem, result);
      try {
        if (!await digAndReadback(bot, here, affected, result, progress.sample))
          throw digBlocked('dig_change_unverified', '目标方块变化未核实');
        check();
        progress.enter('awaiting_pickup');
        await pickup(bot, pos, drops, check, result);
        if (pickupItem) {
          // 游戏拾取包与库存更新可分包到达，沿原工作短等最新的背包读回。
          for (let retry = 0; retry < 8 && drops.entry.selfCount > 0 && (inventory(bot)[pickupItem] ?? 0) <= stock; ++retry) {
            check();
            await delay(50);
          }
          if ((inventory(bot)[pickupItem] ?? 0) <= stock || (args.pickupItem && !drops.entry.selfCount)) {
            const failure = pickupShortfall(drops.entry);
            throw digBlocked(failure.code, failure.message);
          }
        }
      } finally { drops.stop(); }
    }
    if (expected && (inventory(bot)[expected] ?? 0) < args.requiredStock)
      throw digBlocked('pickup_unverified', '实际背包仍缺少所需物品');
    progress.enter('finished');
  } finally {
    const after = inventory(bot);
    if (expected) result.stock.after = after[expected] ?? 0;
    result.inventoryDelta = inventoryDelta(before, after);
    progress.sample();
    job.sampleProgress = undefined;
  }
}

// 为当前玩家提供独立读写技能，重读同一动作只返回已有回执。
// 步骤：验证消息与世界 → 按角色资源接纳技能 → 执行或协作取消 → 原票据读回实际结果。
function partnerService(bot, world, inbox, places, owner, witness = null) {
  let active, previous, closed = false, done = Promise.resolve();
  let sceneWorld, sceneRevision = 0, sceneNext = 0, sceneKey;
  const records = new Map();
  const observations = new Set();
  const withChest = chestWindows(bot);
  const wallSurveys = new Map();
  const checkedRoutes = new Map();
  const built = new BuiltObjects();
  const lease = controllerLease(drainActivities);
  // 核对新工作的来源；一旦接纳，后续聊天不会自行撤销已在执行的工作。
  function current(context) {
    return !closed && inbox.inPresence(context.generation, context.order) && inbox.get(context.generation, context.order) &&
      inbox.read(context.generation, context.order)?.latest === context.order;
  }
  // 从原始收件记录取出当时的现场，玩家后来移动不会改写中心。
  function messageAnchor(generation, sequence) {
    const anchor = Number.isSafeInteger(sequence) && sequence > 0 &&
      inbox.get(generation, sequence)?.sceneAtMessage;
    if (!anchor?.position || anchor.dimension !== bot.game.dimension) throw new Error('原消息的现场位置不可用');
    const { x, y, z } = anchor.position;
    return { anchor, center: bot.entity.position.clone().set(x, y, z) };
  }
  // 撤销正在执行的输入，实际退出仍等待原异步操作结束。
  function cancel() {
    if (!active || active.receipt.exited) return;
    active.cancelled = true;
    active.abort?.abort();
    bot.pathfinder.setGoal(null);
    if (bot.targetDigBlock) bot.stopDigging();
    bot.clearControlStates();
  }
  // 撤销原角色控制和后台观察，已落地的效果由原工作读回。
  function stopActivities() {
    cancel();
    for (const job of observations) { job.cancelled = true; job.abort.abort(); }
  }
  // 等待当前角色的前台动作和后台观察全部归还旧工作号。
  async function drainActivities() {
    stopActivities();
    await Promise.all([done, ...[...observations].map(job => job.done)]);
  }
  // 玩家名单的真实离场立即撤销执行，避免重入期间先执行旧动作。
  const onLeft = player => { if (player.uuid === owner) stopActivities(); };
  bot.on('playerLeft', onLeft);
  // 从私有配对读口定期取样，只有实际视域网格或背包变化才递增修订。
  function scene(generation) {
    if (closed || world.read()?.generation !== generation) return null;
    if (sceneWorld !== generation) {
      sceneWorld = generation;
      sceneRevision = sceneNext = 0;
      sceneKey = undefined;
    }
    const now = performance.now();
    if (now < sceneNext) return sceneRevision;
    const actor = bot.entity.position;
    const player = Object.values(bot.players).find(p => p.uuid === owner)?.entity?.position;
    const center = player && player.distanceTo(actor) <= 24 ? player : actor;
    const key = JSON.stringify([blockSurvey(bot, center), Object.entries(inventory(bot)).sort()]);
    if (sceneKey !== key) {
      if (sceneRevision >= Number.MAX_SAFE_INTEGER) throw new Error('现场修订号已用尽');
      ++sceneRevision;
      sceneKey = key;
    }
    sceneNext = now + 500;
    return sceneRevision;
  }
  // 只在原控制实例、在场周期和维度内读取已结束的掉落范围观察。
  function dropSource(receipt, observation) {
    const original = records.get(workKey(receipt.controller, observation))?.receipt;
    const source = original?.result.dropObservation;
    if (!source || !original.exited || original.result.status !== 'done' || source.generation !== receipt.generation ||
        source.dimension !== bot.game.dimension || !inbox.inPresence(receipt.generation, original.order))
      throw Object.assign(new Error('当前在场周期没有可用的原掉落观察'), { code: 'drop_observation_stale' });
    return source;
  }

  // 复核备料观察并保留首次提交投物的工作号，新的工作只能查回该次效果。
  function handoffSource(receipt, args, result) {
    const record = records.get(workKey(receipt.controller, args.observation));
    const original = record?.receipt;
    const source = original?.result.handoff;
    if (!source || original.skill !== 'observe' || !original.exited || original.result.status !== 'done' ||
        source.generation !== receipt.generation || source.dimension !== bot.game.dimension || !inbox.inPresence(receipt.generation, original.order))
      throw Object.assign(new Error('当前在场周期内没有可用的交物观察'), { code: 'handoff_observation_stale' });
    if (record.transferWork) {
      result.originalWork = record.transferWork;
      throw Object.assign(new Error('这次备料已提交投物，请查原工作的实效'), { code: 'handoff_already_started' });
    }
    return record;
  }

  // 将后继投物绑定到原玩家的取箱交付委托，同一原话更换备料观察仍只允许首次投物。
  // 步骤：核对当前原话与备料来源 → 对照已退出取出的双边凭据 → 找回首次投物认领。
  function chestDelivery(receipt, args, observation, result) {
    for (const order of new Set([receipt.order,observation.order])) {
      const text = inbox.get(receipt.generation,order)?.text;
      if (order === receipt.order && inventoryOrder(text)) throw chestError('chest_delivery_not_authorized','本条取出委托只授权放进伙伴背包');
      if (!deliveryOrder(text)) continue;
      const claim = inbox.chestWork(receipt.generation,order);
      if (!args.withdrawalWork || args.withdrawalWork !== claim?.work || claim.controller !== receipt.controller)
        throw chestError('chest_delivery_source_required','请先查回本条委托的取出工作，再按实取数量交付');
    }
    if (args.withdrawalWork === undefined) return null;
    const original = records.get(workKey(receipt.controller,args.withdrawalWork));
    const proof = chestProof(original);
    if (!original?.receipt.exited || original.receipt.skill !== 'withdraw_item' || !proof || !inbox.inPresence(receipt.generation,proof.sourceMessage) ||
        !inbox.get(receipt.generation,proof.sourceMessage)?.text) throw chestError('chest_delivery_unconfirmed','原取出工作不在当前玩家委托范围');
    const claim = inbox.chestWork(receipt.generation,proof.sourceMessage);
    if (!claim || claim.work !== original.id || claim.controller !== receipt.controller) throw chestError('chest_delivery_unconfirmed','原取出工作身份已失效');
    if (claim.giveWork) {
      result.originalWork = claim.giveWork;
      throw chestError('handoff_already_started','本次取出的物品已经提交交付，请查原投物工作');
    }
    confirmedDelivery(proof,args,observation.result.handoff);
    return claim;
  }

  // 在一次技能调用中只使用当前角色及绑定玩家可见的世界信息。
  async function run(job, args) {
    const r = job.receipt, result = r.result;
    const ownsWindow = () => !closed && lease.owns(r.controller) && inbox.inPresence(r.generation,r.order);
    const openChest = (target,signal,use) => withChest(target,signal,use,ownsWindow);
    let builtSpace;
    if (r.skill === 'reply') Object.assign(result, { sent: false, delivery: 'not_sent' });
    if (r.skill === 'place') result.placementAttempted = result.placed = false;
    if (r.skill === 'fill_target') Object.assign(result, { cells: [], changes: [], placementAttempted: false,
      sourceMessage: args.sourceMessage, taskRevision: args.taskRevision });
    const check = () => {
      if (job.cancelled || closed || !inbox.inPresence(r.generation, r.order)) throw new Error('原工作已中断或游戏绑定已失效');
    };
    try {
      check();
      if (['store_item','withdraw_item','craft','collect','give_item','place','fill_target','pickup_drop'].includes(r.skill)) {
        await withChest.recover();
        check();
      }
      if (['collect', 'place', 'fill_target'].includes(r.skill)) {
        builtSpace = world.space();
        if (!builtSpace) throw new Error('游戏地图空间不可用');
        built.bind(builtSpace);
      }
      const player = Object.values(bot.players).find(p => p.uuid === owner);
      const observation = r.skill === 'observe' ? observationInput(args) : null;
      if (observation?.mode === 'vitals') {
        result.vitals = world.vitals(job.id,r.generation);
        result.vitals.shore = observeShore(bot,result.vitals,(from,stands,inside) => {
          const goal = new goals.GoalCompositeAny(stands.map(p => new goals.GoalBlock(p.x,p.y,p.z)));
          return probePath(bot,from,goal,inside);
        });
      } else if (observation?.mode === 'chest') {
        const { message, item } = observation.options;
        const target = inbox.pointedChest(r.generation,message);
        result.chest = await openChest(target,job.abort.signal,window => ({
          id: job.id, generation: r.generation, owner, actor: bot.entity.uuid ?? bot.player.uuid, message, item,
          ...target, ...chestSample(bot,window,item) }));
      } else if (observation?.mode === 'chest_receipt') {
        const original = records.get(workKey(r.controller,args.work));
        const previous = chestProof(original);
        if (!previous || !original.receipt.exited || previous.generation !== r.generation || previous.dimension !== bot.game.dimension ||
            !inbox.inPresence(r.generation,previous.sourceMessage)) throw chestError('chest_work_unavailable','找不到同一在场周期的原箱内转移工作');
        if (previous.state === 'pending') {
          const after = await openChest(previous,job.abort.signal,window => chestSample(bot,window,previous.item));
          check();
          original.latestChest = reconcileTransfer(previous,after);
        }
        result.chestReceipt = original.latestChest ?? previous;
      } else if (observation?.mode === 'handoff') {
        result.handoff = observeHandoff(bot, owner, observation.options, job.id, r.generation);
      } else if (observation?.mode === 'handoff_receipt') {
        const original = records.get(workKey(r.controller,args.work));
        const action = original?.receipt, transfer = action?.result.transfer;
        if (!action || action.skill !== 'give_item' || action.generation !== r.generation || !inbox.inPresence(r.generation,action.order) ||
            action.result.handoff?.dimension !== bot.game.dimension || !transfer?.submitted || !action.result.receipt || !action.exited)
          throw Object.assign(new Error('当前世界找不到已经退出并提交过投物的原工作'), { code: 'handoff_work_unavailable' });
        await refreshHandoff(original,witness);
        check();
        result.handoffReceipt = handoffSample(original,job.id);
      } else if (observation?.mode === 'drops') {
        result.dropObservation = observeDrops(bot, observation.options, job.id, r.generation);
      } else if (observation?.mode === 'drop') {
        result.dropTarget = readDropTarget(bot, dropSource(r, args.observation), args.entity);
      } else if (observation?.mode === 'detail') {
        const original = records.get(workKey(r.controller, args.observation))?.receipt;
        const source = original?.result.openingObservation;
        if (!source || !original.exited || original.result.status !== 'done' || source.generation !== r.generation ||
            source.dimension !== bot.game.dimension || !inbox.inPresence(r.generation, source.message))
          throw Object.assign(new Error('当前在场周期内没有可展开的原开口观察'), { code: 'observe_detail_unavailable' });
        result.sourceObservation = source;
        result.sampledAtMs = original.result.sampledAtMs;
        result.survey = original.result.survey;
      } else if (observation?.mode === 'wall') {
        const { message, radius } = observation.options;
        const { anchor, center } = messageAnchor(r.generation, message);
        result.wallSurvey = { ...observeWall(bot, anchor, center, radius), id: job.id, message, radius,
          generation: r.generation, dimension: bot.game.dimension };
        remember(wallSurveys, job.key, { ...result.wallSurvey, anchor, center });
      } else if (observation?.mode === 'route') {
        const { observation: id, wall, waypoints } = observation.options;
        const scan = wallSurveys.get(workKey(r.controller, id));
        if (scan?.generation === r.generation && scan.dimension === bot.game.dimension && (!scan.choice || !wall || wall === scan.choice)) {
          const live = observeWall(bot, scan.anchor, scan.center, scan.radius);
          result.route = validateRoute(live, wall ?? scan.choice, waypoints, (a, b, allowed) => readPath(bot, a, b, allowed));
        } else result.route = { status: 'blocked', reasonCode: 'wall_observation_stale' };
        if (result.route.status === 'validated') {
          Object.assign(result.route, { id: job.id, observation: id, sourceMessage: scan.message,
            generation: r.generation, dimension: scan.dimension });
          remember(checkedRoutes, job.key, result.route);
        }
      } else if (observation?.mode === 'scene') {
        await observeWindow(bot, observation.options, result, { check, id: job.id, generation: r.generation, signal: job.abort.signal,
          player: () => Object.values(bot.players).find(p => p.uuid === owner)?.entity,
          messageAnchor: order => messageAnchor(r.generation, order), inventory: () => inventory(bot) });
      } else if (r.skill === 'observe') {
        result.sampledAtMs = Math.floor(performance.now());
        result.position = bot.entity.position.clone();
        result.dimension = bot.game.dimension;
        result.player = player?.entity?.position?.clone() ?? null;
        result.landmarks = Object.fromEntries([...places].filter(([, p]) => p.dimension === bot.game.dimension));
        result.inventory = inventory(bot);
        result.ground = bot.blockAt(bot.entity.position.floored().offset(0, -1, 0))?.name;
        const sight = bot.blockAtCursor(16);
        if (sight) result.selfLook = { block: sight.name, position: sight.position.clone() };
        result.nearby = Object.values(bot.entities).filter(e => e !== bot.entity && e.position.distanceTo(bot.entity.position) <= 16)
          .sort((a, b) => a.position.distanceTo(bot.entity.position) - b.position.distanceTo(bot.entity.position))
          .slice(0, 12).map(e => ({ name: e.username ?? e.name, position: e.position.clone() }));
        let center = args.around === 'player' ? player?.entity?.position : bot.entity.position;
        if (args.message !== undefined) {
          if (args.around || (args.focus !== undefined && !['hit', 'openings', 'objects'].includes(args.focus)))
            throw new Error('请选择原消息和有效的指向查询');
          const { anchor, center: original } = messageAnchor(r.generation, args.message);
          center = original;
          result.anchor = { message: args.message, ...anchor };
          if (args.focus !== 'hit' && args.focus !== 'objects') result.survey = blockSurvey(bot, center, 4);
          result.playerFocus = playerFocus(bot, anchor, center, args.focus,
            { player_nearby: player?.entity?.position, self_nearby: bot.entity.position });
          if (result.survey && !args.block && (args.mode === 'pointed' || args.focus === 'openings'))
            result.openingObservation = { id: job.id, generation: r.generation, dimension: bot.game.dimension,
              message: args.message, coverage: surveyCoverage(result.survey) };
        } else if (args.focus !== undefined) {
          throw new Error('指向查询需要原玩家消息');
        } else if (args.around) result.survey = blockSurvey(bot, center);
        if (args.block) {
          const type = blockType(bot, args.block);
          const blocks = visibleBlocks(bot, type.id, center, 10);
          result.blockSearch = { block: type.name, center: center.floored(), radius: 16, coverage: 'loaded_visible_only' };
          result.visible = blocks.map(block => block.position.clone());
          if (type.name.endsWith('_door')) {
            result.doors = localDoors(bot, type, center);
            result.doorCoverage = 'loaded_local';
          }
        }
        if (previous?.controller === r.controller && previous.action.generation === r.generation) result.lastAction = previous.action;
      } else if (r.skill === 'reply') {
        if (typeof args.text !== 'string' || !args.text.trim() || args.text.length > 220 || /[\r\n\x00]/.test(args.text))
          throw new Error('回复需要一行 1～220 字的文字');
        if (Object.hasOwn(args, 'sourceMessage') && (!Number.isSafeInteger(args.sourceMessage) ||
          args.sourceMessage > r.order || !inbox.inPresence(r.generation, args.sourceMessage) ||
          !inbox.get(r.generation, args.sourceMessage)?.text)) throw new Error('回复缺少同场玩家原话');
        result.delivery = tellOwner(bot, owner, args.text);
        result.sent = result.delivery === 'sent';
        result.reused = false;
        if (result.sent) result.text = args.text;
        if (!result.sent) throw new Error(result.delivery === 'not_sent' ? '原玩家不在场，回复未发出' : '回复的投递结果未知');
      } else if (r.skill === 'route') {
        const route = checkedRoutes.get(workKey(r.controller, args.route));
        const scan = route && wallSurveys.get(workKey(r.controller, route.observation));
        if (!scan || route.generation !== r.generation || route.dimension !== bot.game.dimension ||
          !Array.isArray(args.waypoints) || args.waypoints.length !== 4 || route.waypoints.some((point, i) =>
            ['x', 'y', 'z'].some(axis => point[axis] !== args.waypoints[i]?.[axis])) || !Number.isSafeInteger(args.laps) ||
          args.laps < 1 || args.laps > 8) throw Object.assign(new Error('已核验路线已失效'), { code: 'route_stale' });
        const live = observeWall(bot, scan.anchor, scan.center, scan.radius);
        const checked = validateRoute(live, route.wall, route.waypoints, (a, b, allowed) => readPath(bot, a, b, allowed));
        if (checked.status !== 'validated') throw Object.assign(new Error('现场变化，原路线不可通行'), { code: checked.reasonCode });
        try {
          await runRoute(route, args.laps, result, { check, move: (point, allowed) => walkRouteSegment(bot, point, allowed, check) });
        } finally { result.position = bot.entity.position.clone(); }
      } else if (r.skill === 'move') {
        const named = Object.hasOwn(args, 'place');
        const anchored = Object.hasOwn(args, 'message');
        const exact = args.exact === true;
        if (named && ['x','y','z'].some(k => Object.hasOwn(args,k))) throw new Error('请选择地标或坐标之一');
        if (anchored && (named || ['x','y','z'].some(k => Object.hasOwn(args,k)))) throw new Error('请选择一个移动目标');
        const toPlayer = args.place === 'player';
        if (Object.hasOwn(args,'recipient') && (!toPlayer || args.recipient !== owner))
          throw Object.assign(new Error('会合对象与本次配对玩家不符'), { code: 'meeting_player_unbound' });
        if (toPlayer && (!player?.entity?.position || player.entity.uuid !== owner))
          throw Object.assign(new Error('当前无法定位配对玩家'), { code: 'meeting_player_unavailable' });
        const meeting = toPlayer ? new PlayerMeetingGoal(bot,player) : null;
        if (meeting && !meeting.isValid()) throw Object.assign(new Error('会合目标已离开当前现场'), { code: meeting.issue() });
        const point = toPlayer ? player.entity.position.floored() :
          places.get(args.place) ?? [...places.values()].find(p => p.label === args.place);
        if (named && (!point || (point.dimension && point.dimension !== bot.game.dimension))) throw new Error('地标未知或玩家不在视野');
        const target = anchored ? messageAnchor(r.generation, args.message).center.floored() : named ? point : args;
        const pos = targetPoint(bot, target);
        // 依赖这个落脚点的后续节点需要实际站在目标格。
        if (exact && !clearStand(bot, pos)) throw Object.assign(new Error('目标脚位的支撑或净空不成立'), { code: 'unsafe_stand' });
        const nearby = new goals.GoalNear(pos.x, pos.y, pos.z, anchored ? 3 : 1);
        const dryTarget = anchored || dryStand(bot, pos);
        const conditions = [nearby];
        if (anchored) conditions.push(new goals.GoalY(pos.y));
        if (dryTarget) conditions.push(new DryStandGoal(bot));
        const goal = meeting || (exact ? new goals.GoalBlock(pos.x, pos.y, pos.z) :
          conditions.length > 1 ? new goals.GoalCompositeAll(conditions) : nearby);
        if (anchored) result.sourceMessage = args.message;
        const surface = monitorSurface(bot, result);
        const movement = monitorMove(bot, meeting || pos, goal, result);
        job.sampleProgress = movement.sample;
        try {
          await walk(bot, goal);
          check();
          if (meeting && !meeting.isValid()) throw Object.assign(new Error('原会合对象已不在当前移动范围'), { code: meeting.issue() });
          if (meeting) meeting.hasChanged();
          movement.sample();
          const arrived = bot.entity?.position?.floored();
          const dryArrival = meeting ? dryStand(bot,meeting.entity.position) : dryTarget;
          if (!arrived || !goal.isEnd(arrived) || (dryArrival && bot.entity.isInWater) || (exact && !clearStand(bot, pos))) {
            if (result.progress.path === 'noPath') throw Object.assign(new Error('本次寻路未找到路径'), { code: 'move_no_path' });
            if (result.progress.path === 'timeout') throw Object.assign(new Error('本次寻路在时限内未完成'), { code: 'move_path_timeout' });
            throw Object.assign(new Error('到达站位未核实'), { code: 'arrival_unverified' });
          }
          if (meeting) {
            const position = meeting.entity.position.clone();
            const distance = bot.entity.position.distanceTo(position);
            if (distance > 3) throw Object.assign(new Error('尚未到达玩家当前三格范围'), { code: 'arrival_unverified' });
            result.target = 'player';
            result.meeting = { recipient: owner, entityId: meeting.entity.id, dimension: bot.game.dimension,
              position, distance, sampledAtMs: Math.floor(performance.now()) };
          }
          result.arrived = true;
        } catch (error) {
          if (error.name === 'NoPath') error.code = 'move_no_path';
          if (error.name === 'Timeout') error.code = 'move_path_timeout';
          const issue = meeting?.issue();
          if (issue) error.code = issue;
          throw error;
        } finally {
          if (meeting && bot.pathfinder.goal === meeting) { bot.pathfinder.setGoal(null); bot.clearControlStates(); }
          job.sampleProgress = undefined;
          movement.stop(result.arrived === true);
          surface.stop();
          result.position = bot.entity?.position?.clone() ?? null;
        }
      } else if (r.skill === 'follow') {
        bot.pathfinder.setMovements(walkingMovements(bot));
        await followPlayer(bot, owner, args.distance ?? 3, check, result,
          (from, goal) => probePath(bot, from, goal)?.status, args.probeFirst === true);
      } else if (r.skill === 'collect') await collect(bot, args, check, job, built);
      else if (r.skill === 'pickup_drop') {
        await pickupDrop(bot, args, dropSource(r, args.observation), check, result, {
          go: (stand, allowed) => walk(bot, new goals.GoalBlock(stand.x, stand.y, stand.z), allowed),
          setSampler: sample => { job.sampleProgress = sample; },
          stop() { bot.pathfinder.setGoal(null); bot.clearControlStates(); },
        });
      }
      else if (r.skill === 'give_item') {
        const record = handoffSource(r,args,result);
        const claim = chestDelivery(r,args,record.receipt,result);
        await giveItem(bot,owner,args,record.receipt.result.handoff,check,() => {
          record.transferWork = job.id;
          if (claim) claim.giveWork = job.id;
        },job,witness);
      }
      else if (r.skill === 'store_item' || r.skill === 'withdraw_item') {
        const withdraw = r.skill === 'withdraw_item';
        const existing = inbox.chestWork(r.generation,r.order);
        if (existing) {
          result.originalWork = existing.work;
          throw chestError('chest_already_started','本条委托已接纳，请查原箱内转移工作');
        }
        const original = records.get(workKey(r.controller,args.observation))?.receipt;
        const source = original?.result.chest, order = inbox.get(r.generation,r.order);
        if (!source || original.skill !== 'observe' || !original.exited || original.result.status !== 'done' ||
            source.generation !== r.generation || source.dimension !== bot.game.dimension || source.owner !== owner ||
            !inbox.inPresence(r.generation,source.message) || !inbox.get(r.generation,source.message)?.text ||
            r.order !== args.sourceMessage || !order?.text) throw chestError('chest_observation_stale','需要当前玩家委托和同场原单箱观察');
        args = { ...args,item:args.item.replace(/^minecraft:/,'') };
        const deliverTo = chestOrder(bot,source,inbox.get(r.generation,source.message).text,order.text,inbox.pointedChest(r.generation,r.order),args,withdraw);
        const pending = [...records.values()].find(entry => {
          const proof = chestProof(entry);
          return proof?.state === 'pending' && proof.generation === r.generation && proof.item === source.item &&
            inbox.inPresence(r.generation,proof.sourceMessage);
        });
        if (pending) { result.originalWork = pending.id; throw chestError('chest_recheck_original','先查回原箱内转移工作未决的数量'); }
        await transferItem(bot,openChest,source,args,job,() => {
          const claim = inbox.chestWork(r.generation,r.order,{controller:r.controller,work:job.id});
          if (claim.work !== job.id || claim.controller !== r.controller) {
            result.originalWork = claim.work;
            throw chestError('chest_already_started','本条委托已接纳，请查原箱内转移工作');
          }
        },check,withdraw,deliverTo);
      }
      else if (r.skill === 'craft') await craftPlanks(bot, args, check, result);
      else if (r.skill === 'place' || r.skill === 'fill_target') {
        if (args.sourceMessage !== undefined) {
          if (args.sourceMode === 'approach') {
            const source = inbox.get(r.generation, args.sourceMessage)?.sceneAtMessage;
            if (source?.dimension !== bot.game.dimension) throw new Error('原接近请求不在当前世界');
          } else messageAnchor(r.generation, args.sourceMessage);
          result.sourceMessage = args.sourceMessage;
          result.taskRevision = args.taskRevision;
        }
        if (r.skill === 'fill_target') {
          if (!Array.isArray(args.cells) || !args.cells.length || args.cells.length > 16) throw new Error('原目标方格不可用');
          const points = args.cells.map(cell => targetPoint(bot, cell));
          await placeTarget(bot, points, blockType(bot, args.block), check, walk, result);
        } else await placeBlock(bot, targetPoint(bot, args), blockType(bot, args.block), check, walk, result);
      }
      else throw new Error('未知游戏能力');
      check();
      result.status = 'done';
    } catch (error) {
      result.status = job.cancelled ? 'interrupted' : 'blocked';
      result.reason = String(error.message).slice(0, 240);
      if (['collect', 'pickup_drop', 'give_item', 'store_item', 'withdraw_item', 'place', 'fill_target', 'craft', 'observe', 'route', 'move'].includes(r.skill) && error.code) result.reasonCode = error.code;
    } finally {
      if (builtSpace && world.space() === builtSpace) {
        if (r.skill === 'collect') built.removed(result.changes ?? []);
        if (r.skill === 'place' && result.placed) built.placed(result.blocks);
        if (r.skill === 'fill_target') {
          for (const cell of result.cells.filter(cell => cell.status === 'placed' || cell.status === 'present'))
            built.placed([cell]);
        }
      }
      r.exited = true;
      if (['move', 'route', 'follow', 'collect', 'pickup_drop', 'give_item', 'store_item', 'withdraw_item', 'place', 'fill_target', 'craft'].includes(r.skill))
        previous = { controller: r.controller, action: { ...result, generation: r.generation, order: r.order, skill: r.skill } };
      bot.emit('partnerSkillExit', { work: job.id, ...r, arguments: args });
    }
  }
  return {
    scene,
    // 仅当执行器仍开放时处理收件监听对角色控制权的登记或续期。
    claim(controller) { return !closed && lease.claim(controller); },
    detach: lease.detach,
    // 同一控制实例内的原工作负责轮询、取消和失联收尾，独立查证只读回执。
    // 步骤：核对原工作身份 → 分流只读查证与在途控制 → 按资源条件接纳新工作。
    call(method, id, context, skill, args) {
      if (!Number.isSafeInteger(context?.order) || context.order <= 0 || typeof context.generation !== 'string' ||
          typeof context.controller !== 'string' || !/^[a-f0-9]{32}$/.test(context.controller) ||
          !args || typeof args !== 'object' || Array.isArray(args))
        throw new Error('无效游戏消息身份');
      const key = workKey(context.controller, id);
      const known = records.get(key);
      if (known) {
        const receipt = known.receipt;
        if (receipt.generation !== context.generation || receipt.order !== context.order || skill !== receipt.skill ||
          JSON.stringify(args) !== JSON.stringify(known.args))
          throw new Error('工作身份冲突');
        // 票据收回后的查证只读原记录，游戏工作的失联期限继续计时。
        if (method === 'partner.receipt' && known.latestChest)
          return { ...receipt,result:{...receipt.result,[receipt.skill === 'withdraw_item' ? 'withdrawal' : 'deposit']:known.latestChest} };
        if (method === 'partner.receipt') return originalHandoffReceipt(known,witness);
        if (known === active) {
          known.polled = performance.now();
          known.sampleProgress?.();
          if (method === 'partner.stop') cancel();
        }
        if (method === 'partner.stop' && known.abort && !receipt.exited) {
          known.cancelled = true;
          known.abort.abort();
        }
        return receipt;
      }
      const controlsBody = skill !== 'reply' && skill !== 'observe';
      const chestRead = mode => mode === 'chest' || mode === 'chest_receipt';
      const needsWindow = skill === 'observe' && chestRead(args.mode);
      if (method !== 'partner.begin' || !current(context) || !lease.owns(context.controller) ||
          ((controlsBody || needsWindow) && ((active && !active.receipt.exited) ||
            [...observations].some(job => chestRead(job.args.mode) && !job.receipt.exited))))
        throw new Error('游戏工作已失效或正忙');
      if (skill === 'observe' && observations.size >= 3) throw new Error('观察工作已满');
      const reference = ['store_item','withdraw_item','give_item'].includes(skill) ? args.observation :
        skill === 'observe' && ['handoff_receipt','chest_receipt'].includes(args.mode) ? args.work : null;
      const lookup = reference && records.get(workKey(context.controller,reference));
      const withdrawal = skill === 'give_item' && args.withdrawalWork && records.get(workKey(context.controller,args.withdrawalWork));
      makeReceiptRoom(records,active,context,lookup,(generation,order) => inbox.inPresence(generation,order),withdrawal);
      const job = { id, key, args, receipt: { ...context, skill, exited: false, result: {} }, polled: performance.now(), cancelled: false };
      records.set(key, job);
      if (controlsBody) {
        active = job;
        if (['give_item','store_item','withdraw_item'].includes(skill)) job.abort = new AbortController();
        const timer = setInterval(() => {
          if (active === job && (!lease.owns(context.controller) || !inbox.inPresence(context.generation, context.order) ||
              performance.now() - job.polled > 4000)) cancel();
        }, 150);
        done = run(job, args).finally(() => clearInterval(timer));
      } else if (skill === 'observe') {
        job.abort = new AbortController();
        observations.add(job);
        job.done = run(job, args).finally(() => observations.delete(job));
      } else void run(job, args);
      return job.receipt;
    },
    // 立即关闭接纳，等原工作归还后才允许报告释放。
    async close() {
      closed = true;
      bot.removeListener('playerLeft', onLeft);
      await drainActivities();
    },
  };
}
module.exports = { partnerService };
