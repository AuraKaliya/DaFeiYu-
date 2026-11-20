const { Movements, goals } = require('mineflayer-pathfinder');
const { setTimeout: delay } = require('node:timers/promises');
const { tellOwner } = require('./chat.cjs');
const { blockSurvey } = require('./block-survey.cjs');
const { placeBlock } = require('./placement.cjs');

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

// 仅选择在客户端视线内可见且不在自己脚下的候选方块。
function visible(bot, type) {
  const feet = bot.entity.position.floored();
  return bot.findBlocks({ matching: type.id, maxDistance: 16, count: 96 })
    .map(pos => bot.blockAt(pos)).filter(block => block && bot.canSeeBlock(block) &&
      !(block.position.x === feet.x && block.position.z === feet.z && block.position.y < feet.y));
}

// 游戏动作使用同一份只走路的寻路策略，抵达后再核对目标。
function walk(bot, goal) {
  const moves = new Movements(bot);
  moves.canDig = moves.canOpenDoors = moves.allow1by1towers = moves.allowParkour = false;
  moves.scafoldingBlocks = [];
  bot.pathfinder.setMovements(moves);
  return bot.pathfinder.goto(goal);
}

// 逐块执行本次采集，指定坐标时保持目标，实际破坏与背包增减分别取证。
// 步骤：核对材料和目标 → 走近并选择可采工具 → 读回方块 → 走近掉落并记录背包变化。
async function collect(bot, args, check, result) {
  const type = blockType(bot, args.block);
  const fixed = ['x', 'y', 'z'].some(k => Object.hasOwn(args, k)) ? targetPoint(bot, args) : null;
  if (!Number.isInteger(args.count) || args.count < 1 || args.count > 32 || (fixed && args.count !== 1))
    throw new Error('数量应为 1～32，指定的单格只能采一次');
  const before = inventory(bot);
  result.broken = 0;
  try {
    for (let n = 0; n < args.count; ++n) {
      check();
      const block = fixed ? bot.blockAt(fixed) : visible(bot, type)[0];
      if (!block || block.type !== type.id || !block.diggable) throw new Error('指定目标已变化或附近没有可见的目标方块');
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
      await bot.dig(here, true);
      const after = bot.blockAt(pos);
      if (!after || after.type === type.id) throw new Error('方块变化未核实');
      ++result.broken;
      check();
      await walk(bot, new goals.GoalNear(pos.x, Math.floor(bot.entity.position.y), pos.z, 0));
      // 掉落物有拾取延迟，给实际库存更新留出时间；不以挖掘次数推算入包。
      await delay(650);
    }
  } finally {
    const after = inventory(bot);
    result.inventoryDelta = {};
    for (const name of new Set([...Object.keys(before), ...Object.keys(after)])) {
      const diff = (after[name] ?? 0) - (before[name] ?? 0);
      if (diff) result.inventoryDelta[name] = diff;
    }
  }
}

// 为当前玩家提供独立读写技能，重读同一动作只返回已有回执。
// 步骤：验证消息与世界 → 串行接纳技能 → 执行或协作取消 → 原票据读回实际结果。
function partnerService(bot, world, inbox, places, owner) {
  let active, previous, closed = false, done = Promise.resolve();
  let source = null;
  let sceneWorld, sceneRevision = 0, sceneNext = 0, sceneKey;
  const records = new Map();
  // 核对新工作的来源；一旦接纳，后续聊天不会自行撤销已在执行的工作。
  function current(context) {
    return !closed && world.read()?.generation === context.generation && inbox.get(context.generation, context.order) &&
      inbox.read(context.generation, context.order)?.latest === context.order;
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
    const check = () => { if (job.cancelled || closed || world.read()?.generation !== r.generation) throw new Error('原工作已中断或游戏绑定已失效'); };
    try {
      check();
      const player = Object.values(bot.players).find(p => p.uuid === owner);
      if (r.skill === 'observe') {
        result.position = bot.entity.position.clone();
        result.dimension = bot.game.dimension;
        result.player = player?.entity?.position?.clone() ?? null;
        result.landmarks = Object.fromEntries([...places].filter(([, p]) => p.dimension === bot.game.dimension));
        result.inventory = inventory(bot);
        result.ground = bot.blockAt(bot.entity.position.floored().offset(0, -1, 0))?.name;
        const sight = bot.blockAtCursor(16);
        if (sight) result.lookingAt = { block: sight.name, position: sight.position.clone() };
        result.nearby = Object.values(bot.entities).filter(e => e !== bot.entity && e.position.distanceTo(bot.entity.position) <= 16)
          .sort((a, b) => a.position.distanceTo(bot.entity.position) - b.position.distanceTo(bot.entity.position))
          .slice(0, 12).map(e => ({ name: e.username ?? e.name, position: e.position.clone() }));
        if (args.block) result.visible = visible(bot, blockType(bot, args.block)).slice(0, 10).map(b => b.position.clone());
        if (args.around) result.survey = blockSurvey(bot, args.around === 'player' ? player?.entity?.position : bot.entity.position);
        if (previous) result.lastAction = previous;
      } else if (r.skill === 'reply') {
        if (typeof args.text !== 'string' || !args.text.trim() || args.text.length > 220 || /[\r\n\x00]/.test(args.text))
          throw new Error('回复需要一行 1～220 字的文字');
        const repeated = [...records.values()].some(e => e.receipt !== r && e.receipt.skill === 'reply' &&
          e.receipt.result.sent && e.args.text === args.text);
        result.sent = repeated || tellOwner(bot, owner, args.text);
        result.reused = repeated;
        if (result.sent) result.text = args.text;
        if (!result.sent) throw new Error('原玩家不在场，回复未发出');
      } else if (r.skill === 'move') {
        const named = Object.hasOwn(args, 'place');
        if (named && ['x','y','z'].some(k => Object.hasOwn(args,k))) throw new Error('请选择地标或坐标之一');
        const point = args.place === 'player' ? player?.entity?.position?.floored() :
          places.get(args.place) ?? [...places.values()].find(p => p.label === args.place);
        if (named && (!point || (point.dimension && point.dimension !== bot.game.dimension))) throw new Error('地标未知或玩家不在视野');
        const pos = targetPoint(bot, named ? point : args);
        await walk(bot, new goals.GoalNear(pos.x, pos.y, pos.z, 1));
        check();
        result.position = bot.entity.position.clone();
        if (bot.entity.position.floored().distanceTo(pos) > 1) throw new Error('到达未核实');
      } else if (r.skill === 'collect') await collect(bot, args, check, result);
      else if (r.skill === 'place') {
        await placeBlock(bot, targetPoint(bot, args), blockType(bot, args.block), check, walk, result);
      }
      else throw new Error('未知游戏能力');
      check();
      result.status = 'done';
    } catch (error) {
      result.status = job.cancelled ? 'interrupted' : 'blocked';
      result.reason = String(error.message).slice(0, 240);
    } finally {
      r.exited = true;
      if (['move', 'collect', 'place'].includes(r.skill)) previous = { order: r.order, skill: r.skill, ...result };
      bot.emit('partnerSkillExit', { work: job.id, ...r, arguments: args });
    }
  }
  return {
    scene,
    // 只对同一业务编号启动一次工作，原票据负责轮询、取消以及失联收尾。
    call(method, id, context, skill, args) {
      const key = context.generation + '/' + context.order;
      if (!Number.isSafeInteger(context.order) || context.order <= 0 || typeof context.generation !== 'string')
        throw new Error('无效游戏消息身份');
      if (method === 'partner.begin' && current(context) && source !== key && (!active || active.receipt.exited)) {
        if (active?.receipt.generation !== context.generation) previous = null;
        source = key;
        records.clear();
      }
      const known = records.get(id);
      if (known) {
        if (source !== key || skill !== known.receipt.skill || (method === 'partner.begin' && JSON.stringify(args) !== JSON.stringify(known.args)))
          throw new Error('工作身份冲突');
        if (known === active) {
          known.polled = performance.now();
          if (method === 'partner.stop') cancel();
        }
        return known.receipt;
      }
      if (method !== 'partner.begin' || !current(context) || (active && !active.receipt.exited) || records.size >= 64)
        throw new Error('游戏工作已失效或正忙');
      const job = { id, args, receipt: { ...context, skill, exited: false, result: {} }, polled: performance.now(), cancelled: false };
      records.set(id, job);
      active = job;
      const timer = setInterval(() => { if (world.read()?.generation !== context.generation || performance.now() - job.polled > 4000) cancel(); }, 150);
      done = run(job, args).finally(() => clearInterval(timer));
      return job.receipt;
    },
    // 立即关闭接纳，等原工作归还后才允许报告释放。
    async close() { closed = true; cancel(); await done; },
  };
}
module.exports = { partnerService };
