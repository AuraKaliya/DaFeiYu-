const { Movements, goals } = require('mineflayer-pathfinder');
const { setTimeout: delay } = require('node:timers/promises');
const { tellOwner } = require('./chat.cjs');
const { blockSurvey, observedDoors } = require('./block-survey.cjs');
const { playerFocus } = require('./player-focus.cjs');
const { observeWindow } = require('./partner-observe.cjs');
const { placeBlock, placeTarget } = require('./placement.cjs');
const { followPlayer } = require('./follow.cjs');
const { fromGround, collectReadback, pickupStand, nearbyDrop } = require('./collection.cjs');
const { dryStand, DryStandGoal, monitorSurface } = require('./navigation.cjs');

// 汇总实际背包里的物品，合并不同槽位内的同名堆叠。
function inventory(bot) {
  const stock = {};
  for (const item of bot.inventory.items()) stock[item.name] = (stock[item.name] ?? 0) + item.count;
  return stock;
}

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
function visible(bot, type, center = bot.entity.position) {
  const feet = bot.entity.position.floored();
  return bot.findBlocks({ matching: type.id, point: center, maxDistance: 16, count: 96 })
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

// 一次移动和持续跟随都使用只走路的寻路策略。
function prepareWalking(bot) {
  const moves = new Movements(bot);
  moves.canDig = moves.canOpenDoors = moves.allow1by1towers = moves.allowParkour = false;
  moves.scafoldingBlocks = [];
  bot.pathfinder.setMovements(moves);
}

// 走到固定目标并等待寻路器真实退出。
function walk(bot, goal) {
  prepareWalking(bot);
  return bot.pathfinder.goto(goal);
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

// 逐块执行本次采集，指定坐标时保持目标，实际破坏与背包增减分别取证。
// 步骤：核对材料和目标 → 走近并选择可采工具 → 读回方块 → 走近掉落并记录背包变化。
async function collect(bot, args, check, result) {
  const type = blockType(bot, args.block);
  const fixed = ['x', 'y', 'z'].some(k => Object.hasOwn(args, k)) ? targetPoint(bot, args) : null;
  if (!Number.isInteger(args.count) || args.count < 1 || args.count > 32 || (fixed && args.count !== 1))
    throw new Error('数量应为 1～32，指定的单格只能采一次');
  const before = inventory(bot);
  const expected = args.item?.replace(/^minecraft:/, '');
  if (Object.hasOwn(args, 'item') && (!Object.hasOwn(bot.registry.itemsByName, expected) ||
      args.requiredStock !== args.count || args.source !== 'ground'))
    throw new Error('地面取材需要已知物品和目标库存');
  const missing = expected ? Math.max(0, args.requiredStock - (before[expected] ?? 0)) : args.count;
  if (expected) result.stock = { item: expected, before: before[expected] ?? 0, required: args.requiredStock };
  result.broken = 0;
  result.changes = [];
  const allowed = block => args.source !== 'ground' || fromGround(bot, block);
  try {
    for (let n = 0; n < missing; ++n) {
      check();
      const stock = expected ? (inventory(bot)[expected] ?? 0) : 0;
      if (expected && stock >= args.requiredStock) break;
      const block = fixed ? bot.blockAt(fixed) : visible(bot, type).find(allowed);
      if (!block || block.type !== type.id || !block.diggable || !allowed(block))
        throw new Error(args.source === 'ground' ? '未读到符合取材范围的地面' : '指定目标已变化或附近没有可见的目标方块');
      const pos = block.position;
      if (pos.equals(bot.entity.position.floored().offset(0, -1, 0))) throw new Error('目标支撑着角色，请先移动');
      if (!bot.canDigBlock(block) || !bot.canSeeBlock(block)) await walk(bot, new goals.GoalLookAtBlock(pos, bot.world));
      check();
      const items = bot.inventory.items().filter(item => block.canHarvest(item.type));
      const best = items.reduce((item, next) => !item || block.digTime(next.type) < block.digTime(item.type) ? next : item, null);
      if (best && (!block.canHarvest(bot.heldItem?.type) || block.digTime(best.type) < block.digTime(bot.heldItem?.type)))
        await bot.equip(best, 'hand');
      check();
      const here = bot.blockAt(pos);
      if (!here || here.type !== type.id || !here.canHarvest(bot.heldItem?.type) || !bot.canDigBlock(here) || !bot.canSeeBlock(here))
        throw new Error('目标、距离或工具已不满足采掘条件');
      if (pos.equals(bot.entity.position.floored().offset(0, -1, 0))) throw new Error('走近后目标位于脚下');
      if (!allowed(here)) throw new Error('目标已不符合地面取材范围');
      const readback = collectReadback(bot, here);
      const known = new Set(Object.keys(bot.entities));
      try { await bot.dig(here, true); }
      finally { await readback(result.changes); }
      const after = bot.blockAt(pos);
      if (!after || after.type === type.id) throw new Error('方块变化未核实');
      ++result.broken;
      check();
      await pickup(bot, pos, known, check, result);
      if (expected && (inventory(bot)[expected] ?? 0) <= stock)
        throw new Error('没有读到所需物品入包，已停止取材');
    }
    if (expected && (inventory(bot)[expected] ?? 0) < args.requiredStock) throw new Error('实际背包仍缺少所需物品');
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
  // 核对新工作的来源；一旦接纳，后续聊天不会自行撤销已在执行的工作。
  function current(context) {
    return !closed && world.read()?.generation === context.generation && inbox.get(context.generation, context.order) &&
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
    if (r.skill === 'place') result.placementAttempted = result.placed = false;
    if (r.skill === 'fill_target') Object.assign(result, { cells: [], changes: [], placementAttempted: false,
      sourceMessage: args.sourceMessage, taskRevision: args.taskRevision });
    const check = () => { if (job.cancelled || closed || world.read()?.generation !== r.generation) throw new Error('原工作已中断或游戏绑定已失效'); };
    try {
      check();
      const player = Object.values(bot.players).find(p => p.uuid === owner);
      if (r.skill === 'observe' && ['sources','radius','detail','samples','verify'].some(key => Object.hasOwn(args, key))) {
        await observeWindow(bot, args, result, { check, id: job.id, generation: r.generation, signal: job.abort.signal,
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
          if (args.around) throw new Error('请选择原消息或当前观察中心');
          const { anchor, center: original } = messageAnchor(r.generation, args.message);
          center = original;
          result.anchor = { message: args.message, ...anchor };
          result.survey = blockSurvey(bot, center, 4);
          result.playerFocus = playerFocus(bot, anchor, center);
        } else if (args.around) result.survey = blockSurvey(bot, center);
        if (args.block) {
          const type = blockType(bot, args.block);
          const blocks = visible(bot, type, center).slice(0, 10);
          result.blockSearch = { block: type.name, center: center.floored(), radius: 16, coverage: 'loaded_visible_only' };
          result.visible = blocks.map(block => block.position.clone());
          if (type.name.endsWith('_door')) {
            result.doors = localDoors(bot, type, center);
            result.doorCoverage = 'loaded_local';
          }
        }
        if (previous?.generation === r.generation) result.lastAction = previous;
      } else if (r.skill === 'reply') {
        if (typeof args.text !== 'string' || !args.text.trim() || args.text.length > 220 || /[\r\n\x00]/.test(args.text))
          throw new Error('回复需要一行 1～220 字的文字');
        const repeated = [...records.values()].some(e => e.receipt !== r && e.receipt.skill === 'reply' &&
          e.receipt.generation === r.generation && e.receipt.order === r.order && e.receipt.result.sent && e.args.text === args.text);
        result.sent = repeated || tellOwner(bot, owner, args.text);
        result.reused = repeated;
        if (result.sent) result.text = args.text;
        if (!result.sent) throw new Error('原玩家不在场，回复未发出');
      } else if (r.skill === 'move') {
        const named = Object.hasOwn(args, 'place');
        const anchored = Object.hasOwn(args, 'message');
        if (named && ['x','y','z'].some(k => Object.hasOwn(args,k))) throw new Error('请选择地标或坐标之一');
        if (anchored && (named || ['x','y','z'].some(k => Object.hasOwn(args,k)))) throw new Error('请选择一个移动目标');
        const point = args.place === 'player' ? player?.entity?.position?.floored() :
          places.get(args.place) ?? [...places.values()].find(p => p.label === args.place);
        if (named && (!point || (point.dimension && point.dimension !== bot.game.dimension))) throw new Error('地标未知或玩家不在视野');
        const target = anchored ? messageAnchor(r.generation, args.message).center.floored() : named ? point : args;
        const pos = targetPoint(bot, target);
        const nearby = new goals.GoalNear(pos.x, pos.y, pos.z, anchored ? 3 : 1);
        const dryTarget = anchored || dryStand(bot, pos);
        const conditions = [nearby];
        if (anchored) conditions.push(new goals.GoalY(pos.y));
        if (dryTarget) conditions.push(new DryStandGoal(bot));
        const goal = conditions.length > 1 ? new goals.GoalCompositeAll(conditions) : nearby;
        if (anchored) result.sourceMessage = args.message;
        const surface = monitorSurface(bot, result);
        try { await walk(bot, goal); }
        finally { surface.stop(); result.position = bot.entity.position.clone(); }
        check();
        const arrived = bot.entity.position.floored();
        if (!goal.isEnd(arrived) || (dryTarget && bot.entity.isInWater))
          throw new Error('到达站位未核实');
        if (args.place === 'player') result.target = 'player';
      } else if (r.skill === 'follow') {
        prepareWalking(bot);
        await followPlayer(bot, owner, args.distance ?? 3, check, result);
      } else if (r.skill === 'collect') await collect(bot, args, check, result);
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
    } finally {
      r.exited = true;
      if (['move', 'follow', 'collect', 'place', 'fill_target'].includes(r.skill))
        previous = { ...result, generation: r.generation, order: r.order, skill: r.skill };
      bot.emit('partnerSkillExit', { work: job.id, ...r, arguments: args });
    }
  }
  return {
    scene,
    // 只对同一业务编号启动一次工作，原票据负责轮询、取消以及失联收尾。
    call(method, id, context, skill, args) {
      if (!Number.isSafeInteger(context.order) || context.order <= 0 || typeof context.generation !== 'string')
        throw new Error('无效游戏消息身份');
      const known = records.get(id);
      if (known) {
        const receipt = known.receipt;
        if (receipt.generation !== context.generation || receipt.order !== context.order || skill !== receipt.skill ||
          (method === 'partner.begin' && JSON.stringify(args) !== JSON.stringify(known.args)))
          throw new Error('工作身份冲突');
        if (known === active) {
          known.polled = performance.now();
          if (method === 'partner.stop') cancel();
        }
        if (method === 'partner.stop' && known.abort && !receipt.exited) {
          known.cancelled = true;
          known.abort.abort();
        }
        return receipt;
      }
      const controlsBody = skill !== 'reply' && skill !== 'observe';
      if (method !== 'partner.begin' || !current(context) || (controlsBody && active && !active.receipt.exited))
        throw new Error('游戏工作已失效或正忙');
      if (skill === 'observe' && observations.size >= 3) throw new Error('观察工作已满');
      // 只回收已经退出且原消息不可能再次接纳的票据。
      if (records.size >= 64) {
        const reclaimable = [...records].find(([, entry]) => entry !== active && entry.receipt.exited &&
          (entry.receipt.generation !== context.generation || entry.receipt.order !== context.order));
        if (!reclaimable) throw new Error('游戏工作回执已满');
        records.delete(reclaimable[0]);
      }
      const job = { id, args, receipt: { ...context, skill, exited: false, result: {} }, polled: performance.now(), cancelled: false };
      records.set(id, job);
      if (controlsBody) {
        active = job;
        const timer = setInterval(() => {
          if (active === job && (world.read()?.generation !== context.generation || performance.now() - job.polled > 4000)) cancel();
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
      cancel();
      for (const job of observations) { job.cancelled = true; job.abort.abort(); }
      await Promise.all([done, ...[...observations].map(job => job.done)]);
    },
  };
}
module.exports = { partnerService };
