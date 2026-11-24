const { Movements, goals } = require('mineflayer-pathfinder');
const { setTimeout: delay } = require('node:timers/promises');
const { tellOwner } = require('./chat.cjs');
const { blockSurvey, surveyCoverage, observedDoors } = require('./block-survey.cjs');
const { playerFocus } = require('./player-focus.cjs');
const { observationInput, observeWindow } = require('./partner-observe.cjs');
const { observeDrops } = require('./dropped-items.cjs');
const { observeWall, surface, validateRoute, runRoute } = require('./partner-route.cjs');
const { placeBlock, placeTarget } = require('./placement.cjs');
const { craftPlanks } = require('./crafting.cjs');
const { followPlayer } = require('./follow.cjs');
const { fromGround, groundTypes, requiresCollectionApproval, affectedByDig, collectReadback, pickupStand, nearbyDrop,
  matchesDigReference } = require('./collection.cjs');
const { BuiltObjects } = require('./built-objects.cjs');
const { controllerLease } = require('./controller.cjs');
const { dryStand, clearStand, DryStandGoal, digReachable, DigStandGoal, monitorSurface } = require('./navigation.cjs');
const { monitorMove } = require('./move-progress.cjs');
const { belowFeet } = require('./world.cjs');

// 汇总实际背包里的物品，合并不同槽位内的同名堆叠。
function inventory(bot) {
  const stock = {};
  for (const item of bot.inventory.items()) stock[item.name] = (stock[item.name] ?? 0) + item.count;
  return stock;
}

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

// 在指定中心附近搜索，仅返回角色可见且不在自己脚下的候选方块。
function visible(bot, matching, center = bot.entity.position) {
  const feet = bot.entity.position.floored();
  return bot.findBlocks({ matching, point: center, maxDistance: 16, count: 96 })
    .map(pos => bot.blockAt(pos)).filter(block => block && bot.canSeeBlock(block) &&
      !(block.position.x === feet.x && block.position.z === feet.z && block.position.y < feet.y));
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
async function pickup(bot, point, known, check, result) {
  let expired = false, tracked;
  const timer = setTimeout(() => { expired = true; bot.pathfinder.setGoal(null); bot.clearControlStates(); }, 6000);
  try {
    await delay(400);
    for (let attempt = 0; attempt < 3; ++attempt) {
      check();
      if (expired) throw new Error('拾取超时');
      if (tracked && bot.entities[tracked.id] !== tracked) return;
      tracked ??= nearbyDrop(bot, point, known);
      const stand = pickupStand(bot, tracked?.position.floored() ?? point);
      if (!stand) throw new Error('掉落物附近没有已知站位');
      if (!stand.equals(bot.entity.position.floored())) await walk(bot, new goals.GoalBlock(stand.x, stand.y, stand.z));
      await delay(450);
      if (!tracked && !nearbyDrop(bot, point, known)) return;
    }
    if (tracked && bot.entities[tracked.id] === tracked) result.pickupIssue = '未读到附近掉落物的拾取';
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
async function collect(bot, args, check, result, built) {
  const fixed = ['x', 'y', 'z'].some(k => Object.hasOwn(args, k)) ? targetPoint(bot, args) : null;
  if (!Number.isInteger(args.count) || args.count < 1 || args.count > 32 || (fixed && args.count !== 1))
    throw new Error('数量应为 1～32，指定的单格只能采一次');
  const referenceChanged = () => digBlocked('dig_reference_changed', '目标下方的原参照已变化');
  if (args.digReference && (!fixed || !matchesDigReference(bot, fixed, args.digReference))) throw referenceChanged();
  const before = inventory(bot);
  const expected = args.item?.replace(/^minecraft:/, '');
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
  try {
    for (let n = 0; n < missing; ++n) {
      check();
      const stock = expected ? (inventory(bot)[expected] ?? 0) : 0;
      if (expected && stock >= args.requiredStock) break;
      if (!matching.length) throw digBlocked('dig_source_missing', '当前地面没有这种物品的自然来源');
      const block = fixed ? bot.blockAt(fixed) : visible(bot, matching).find(allowed);
      if (!block) throw fixed ? digBlocked('dig_target_unloaded', '目标格尚未加载') :
        digBlocked('dig_source_missing', '当前视野内没有符合取材范围的方块');
      if (!matching.includes(block.type)) {
        if (!fixed) throw digBlocked('dig_source_missing', '读取的候选已不是所需材质');
        result.targetBlock = { x: fixed.x, y: fixed.y, z: fixed.z, expected: type.name, observed: block.name, phase: 'initial' };
        throw digBlocked('dig_material_mismatch', '目标格读到' + block.name + '，请求材质是' + type.name);
      }
      if (!block.diggable) throw digBlocked('dig_not_breakable', '该方块无法采掘');
      if (!permitsDig(affectedByDig(bot, block))) throw digBlocked('dig_protected', '目标属于已验收的对象或其支撑，需要指定拆改');
      if (!allowed(block)) throw digBlocked('dig_outside_scope', '目标不在地面取材范围内');
      checkApproval(affectedByDig(bot, block));
      const pos = block.position;
      if (pos.equals(belowFeet(bot.entity.position)))
        throw digBlocked('dig_stand_support', '目标支撑着角色，请先移动');
      if (!digReachable(bot, pos, bot.entity.position)) {
        try { await walk(bot, new DigStandGoal(bot, pos)); }
        catch (error) {
          check();
          throw digBlocked('dig_path_blocked', '无法走到目标的采掘站位：' + error.message);
        }
        finally { result.approachPosition = bot.entity.position.clone(); }
      }
      check();
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
      const readback = collectReadback(bot, here, affected);
      const known = new Set(Object.keys(bot.entities));
      try { await bot.dig(here, true); }
      finally { await readback(result.changes); }
      const after = bot.blockAt(pos);
      if (!after || after.type === block.type) throw new Error('方块变化未核实');
      ++result.broken;
      check();
      await pickup(bot, pos, known, check, result);
      if (expected && (inventory(bot)[expected] ?? 0) <= stock)
        throw digBlocked('pickup_unverified', '没有读到所需物品入包，已停止取材');
    }
    if (expected && (inventory(bot)[expected] ?? 0) < args.requiredStock)
      throw digBlocked('pickup_unverified', '实际背包仍缺少所需物品');
  } finally {
    const after = inventory(bot);
    if (expected) result.stock.after = after[expected] ?? 0;
    result.inventoryDelta = {};
    for (const name of new Set([...Object.keys(before), ...Object.keys(after)])) {
      const diff = (after[name] ?? 0) - (before[name] ?? 0);
      if (diff) result.inventoryDelta[name] = diff;
    }
  }
}

// 为当前玩家提供独立读写技能，重读同一动作只返回已有回执。
// 步骤：验证消息与世界 → 按角色资源接纳技能 → 执行或协作取消 → 原票据读回实际结果。
function partnerService(bot, world, inbox, places, owner) {
  let active, previous, closed = false, done = Promise.resolve();
  let sceneWorld, sceneRevision = 0, sceneNext = 0, sceneKey;
  const records = new Map();
  const observations = new Set();
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
  // 在一次技能调用中只使用当前角色及绑定玩家可见的世界信息。
  async function run(job, args) {
    const r = job.receipt, result = r.result;
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
      if (['collect', 'place', 'fill_target'].includes(r.skill)) {
        builtSpace = world.space();
        if (!builtSpace) throw new Error('游戏地图空间不可用');
        built.bind(builtSpace);
      }
      const player = Object.values(bot.players).find(p => p.uuid === owner);
      const observation = r.skill === 'observe' ? observationInput(args) : null;
      if (observation?.mode === 'drops') {
        result.dropObservation = observeDrops(bot, observation.options, job.id, r.generation);
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
          const blocks = visible(bot, type.id, center).slice(0, 10);
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
        const original = [...records.values()].find(e => e.receipt !== r && e.receipt.skill === 'reply' &&
          e.receipt.generation === r.generation && e.receipt.order === r.order && e.args.text === args.text &&
          ['sent', 'unknown'].includes(e.receipt.result.delivery));
        result.delivery = original?.receipt.result.delivery ?? tellOwner(bot, owner, args.text);
        result.sent = result.delivery === 'sent';
        result.reused = Boolean(original);
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
        const point = args.place === 'player' ? player?.entity?.position?.floored() :
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
        const goal = exact ? new goals.GoalBlock(pos.x, pos.y, pos.z) :
          conditions.length > 1 ? new goals.GoalCompositeAll(conditions) : nearby;
        if (anchored) result.sourceMessage = args.message;
        const surface = monitorSurface(bot, result);
        const movement = monitorMove(bot, pos, goal, result);
        job.sampleMove = movement.sample;
        try {
          await walk(bot, goal);
          check();
          movement.sample();
          const arrived = bot.entity?.position?.floored();
          if (!arrived || !goal.isEnd(arrived) || (dryTarget && bot.entity.isInWater) || (exact && !clearStand(bot, pos))) {
            if (result.progress.path === 'noPath') throw Object.assign(new Error('本次寻路未找到路径'), { code: 'move_no_path' });
            if (result.progress.path === 'timeout') throw Object.assign(new Error('本次寻路在时限内未完成'), { code: 'move_path_timeout' });
            throw Object.assign(new Error('到达站位未核实'), { code: 'arrival_unverified' });
          }
          result.arrived = true;
          if (args.place === 'player') result.target = 'player';
        } catch (error) {
          if (error.name === 'NoPath') error.code = 'move_no_path';
          if (error.name === 'Timeout') error.code = 'move_path_timeout';
          throw error;
        } finally {
          job.sampleMove = undefined;
          movement.stop(result.arrived === true);
          surface.stop();
          result.position = bot.entity?.position?.clone() ?? null;
        }
      } else if (r.skill === 'follow') {
        bot.pathfinder.setMovements(walkingMovements(bot));
        await followPlayer(bot, owner, args.distance ?? 3, check, result,
          (from, goal) => probePath(bot, from, goal)?.status, args.probeFirst === true);
      } else if (r.skill === 'collect') await collect(bot, args, check, result, built);
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
      if (['collect', 'place', 'fill_target', 'craft', 'observe', 'route', 'move'].includes(r.skill) && error.code) result.reasonCode = error.code;
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
      if (['move', 'route', 'follow', 'collect', 'place', 'fill_target', 'craft'].includes(r.skill))
        previous = { controller: r.controller, action: { ...result, generation: r.generation, order: r.order, skill: r.skill } };
      bot.emit('partnerSkillExit', { work: job.id, ...r, arguments: args });
    }
  }
  return {
    scene,
    // 仅当执行器仍开放时处理收件监听对角色控制权的登记或续期。
    claim(controller) { return !closed && lease.claim(controller); },
    detach: lease.detach,
    // 同一控制实例内的原工作负责轮询、取消及失联收尾。
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
        if (known === active) {
          known.polled = performance.now();
          known.sampleMove?.();
          if (method === 'partner.stop') cancel();
        }
        if (method === 'partner.stop' && known.abort && !receipt.exited) {
          known.cancelled = true;
          known.abort.abort();
        }
        return receipt;
      }
      const controlsBody = skill !== 'reply' && skill !== 'observe';
      if (method !== 'partner.begin' || !current(context) || !lease.owns(context.controller) ||
          (controlsBody && active && !active.receipt.exited))
        throw new Error('游戏工作已失效或正忙');
      if (skill === 'observe' && observations.size >= 3) throw new Error('观察工作已满');
      // 只回收已经退出且原消息不可能再次接纳的票据。
      if (records.size >= 64) {
        const reclaimable = [...records].find(([, entry]) => entry !== active && entry.receipt.exited &&
          (entry.receipt.generation !== context.generation || entry.receipt.order !== context.order ||
            entry.receipt.controller !== context.controller));
        if (!reclaimable) throw new Error('游戏工作回执已满');
        records.delete(reclaimable[0]);
      }
      const job = { id, key, args, receipt: { ...context, skill, exited: false, result: {} }, polled: performance.now(), cancelled: false };
      records.set(key, job);
      if (controlsBody) {
        active = job;
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
