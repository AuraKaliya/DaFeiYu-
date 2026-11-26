const { chestSample, chestError } = require('./single-chest.cjs');
const { setTimeout: delay } = require('node:timers/promises');

// 读取明确委托中的总数，支持数字和带十位的中文数量。
function quantity(text) {
  if (/^\d+$/.test(text)) return Number(text);
  const digits = '零一二三四五六七八九';
  const [high,low] = text.replace(/两/g,'二').split('十');
  if (low === undefined) return high.length === 1 ? digits.indexOf(high) : -1;
  if (text.split('十').length !== 2 || high.length > 1 || low.length > 1) return -1;
  return (high ? digits.indexOf(high) : 1)*10 + (low ? digits.indexOf(low) : 0);
}

// 将当前委托的方向、物品、数量和去向与原消息及同一个普通箱相配。
function chestOrder(bot, source, pointing, instruction, aim, args, withdraw = false) {
  if (Object.keys(args).length !== (withdraw ? 5 : 4) || (withdraw && args.destination !== 'companion_inventory') ||
      args.observation !== source.id || !Number.isSafeInteger(args.sourceMessage) ||
      args.sourceMessage < source.message || args.item !== source.item || !Number.isSafeInteger(args.count) || args.count < 1 || args.count > 2304)
    throw chestError('chest_order_mismatch','物品、数量或去向与指定范围不符');
  const line = instruction.replace(/\s+/g,'');
  const match = withdraw
    ? /^(?:请(?:你)?|麻烦(?:你)?)?(?:帮我)?从(?<box>左箱|右箱|左边的箱子|右边的箱子|这个箱子?|那个箱子?)(?:里|中)?(?:取出|拿出)(?<count>[0-9一二两三四五六七八九十]+)(?:块|个)?(?<item>[\p{L}\w:]+?)[，,]?(?:放进|放入|放到|收到)(?:你的|伙伴(?:的)?|你自己的)背包(?:里|中)?[。！!]?$/u.exec(line) ??
      /^(?:请(?:你)?|麻烦(?:你)?)?(?:把|将)(?<box>左箱|右箱|左边的箱子|右边的箱子|这个箱子?|那个箱子?)(?:里|中)的(?<count>[0-9一二两三四五六七八九十]+)(?:块|个)?(?<item>[\p{L}\w:]+?)(?:取到|拿到|取进|收到)(?:你的|伙伴(?:的)?|你自己的)背包(?:里|中)?[。！!]?$/u.exec(line)
    : /^(?:请(?:你)?|麻烦(?:你)?)?(?:帮我)?(?:把|将)?(?<bound>这|那)?(?<count>[0-9一二两三四五六七八九十]+)(?:块|个)?(?<item>[\p{L}\w:]*?)(?:存进|存入|放进|放入|放到|存到)(?<box>左箱|右箱|左边的箱子|右边的箱子|这个箱子|那个箱子|这个箱|那个箱)[。！!]?$/u.exec(line);
  const names = [source.item,'minecraft:'+source.item,bot.registry.itemsByName[source.item]?.displayName,
    ...(source.item === 'cobblestone' ? ['圆石','圓石'] : [])];
  const fields = match?.groups;
  if (!fields || quantity(fields.count) !== args.count || (fields.item ? !names.includes(fields.item) : !fields.bound))
    throw chestError('chest_explicit_order_required','需要当前玩家明确指定方向、物品、数量和去向');
  const side = text => text.match(/(左|右)(?:边的箱子|箱)/)?.[1];
  const named = side(fields.box), selected = side(pointing);
  const aimed = aim && aim.dimension === source.dimension && ['x','y','z'].every(key => aim.cell[key] === source.cell[key]);
  if ((named && (selected !== named || source.message >= args.sourceMessage)) || (aim && !aimed) || (!named && !aimed))
    throw chestError('chest_target_mismatch','当前委托没有指向已核对的同一个箱子');
}

// 用两次完整服务端窗口按原方向核对双边数量，只结算提交范围内的一致净变化。
function reconcileTransfer(proof, after) {
  const sign = proof.direction === 'withdraw' ? -1 : 1;
  const amount = sign*(after.container.stored-proof.before.container.stored);
  if (after.container.status !== 'sampled' || after.stock.status !== 'sampled' || amount < 0 || amount > proof.limit ||
      amount !== sign*(proof.before.stock.stored-after.stock.stored) || (amount > 0 && !proof.attempted)) return proof;
  return { ...proof,state:'verified',after,[sign < 0 ? 'withdrawn' : 'stored']:amount || 0,remaining:proof.requested-amount };
}

// 生成受原工作及单箱窗口约束的点击入口。
function checkedClicks(bot, window, proof, check) {
  // 每次点击前复核窗口和普通栈覆盖，客户端预测只用于定位下一槽。
  return async (slot,button,mode) => {
    check();
    if (bot.currentWindow !== window) throw chestError('chest_window_closed','搬运途中原窗口已关闭');
    const sample = chestSample(bot,window,proof.item);
    if (sample.container.status !== 'sampled' || sample.stock.status !== 'sampled')
      throw chestError('chest_slots_changed','点击前槽位覆盖或普通物品栈已变化');
    proof.attempted = true;
    await bot.clickWindow(slot,button,mode);
    if (bot.currentWindow !== window) throw chestError('chest_window_closed','搬运途中原窗口已关闭');
  };
}

// 将背包普通栈存进指定箱子，拆栈时将剩余物品归还原槽。
async function moveIntoChest(window, proof, click) {
  let remaining = proof.limit;
  for (let src = 27; src < 63 && remaining > 0; ++src) {
    const stack = window.slots[src];
    if (stack?.name !== proof.item) continue;
    const take = Math.min(remaining,stack.count);
    if (take === stack.count) {
      await click(src,0,1);
    } else {
      await click(src,0,0);
      let units = take;
      for (let dest = 0; dest < 27 && units; ++dest) {
        const entry = window.slots[dest];
        if (entry && entry.name !== proof.item) continue;
        const pieces = Math.min(units,proof.before.container.stackSize-(entry?.count ?? 0));
        for (let i = 0; i < pieces; ++i) await click(dest,1,0);
        units -= pieces;
      }
      if (window.selectedItem) {
        if (window.slots[src]) throw chestError('chest_slots_changed','归还手中余量时原槽已被占用');
        await click(src,0,0);
      }
      if (units) throw chestError('chest_slots_changed','存放时箱内可用槽位已变化');
    }
    remaining -= take;
  }
}

// 从指定箱取入伙伴背包，每一次可能随关窗归入背包的光标数量都受剩余授权约束。
// 步骤：优先选择可整栈转移的源槽 → 必要时取半栈分配 → 核对本次实际槽差并扣减预算。
async function moveOutOfChest(window, proof, click) {
  let moved = 0;
  while (moved < proof.limit) {
    const left = proof.limit-moved, authorized = proof.requested-moved;
    const slots = window.slots.slice(0,27);
    const full = slots.findIndex(stack => stack?.name === proof.item && stack.count <= authorized);
    if (full >= 0) {
      const count = slots[full].count;
      await click(full,0,1);
      const delta = count-(window.slots[full]?.count ?? 0);
      if (delta <= 0 || delta > left) throw chestError('chest_slots_changed','取出时伙伴背包的可用空间已变化');
      moved += delta;
      continue;
    }
    const src = slots.findIndex(stack => stack?.name === proof.item && Math.ceil(stack.count/2) <= left);
    if (src < 0) return 'chest_split_unavailable';
    const count = slots[src].count, half = Math.ceil(count/2);
    const take = half === left ? left : Math.min(half,2*left-count);
    await click(src,1,0);
    if (window.selectedItem?.name !== proof.item || window.selectedItem.count !== half)
      throw chestError('chest_slots_changed','取出时原箱槽位已变化');
    let units = take;
    for (let dest = 27; dest < 63 && units; ++dest) {
      const entry = window.slots[dest];
      if (entry && entry.name !== proof.item) continue;
      const pieces = Math.min(units,proof.before.stock.stackSize-(entry?.count ?? 0));
      for (let i = 0; i < pieces; ++i) await click(dest,1,0);
      units -= pieces;
    }
    if (units) throw chestError('chest_slots_changed','取出时伙伴背包的槽位已变化');
    if (window.selectedItem) await click(src,0,0);
    moved += take;
  }
}

// 固定原工作的双边基线，转移后重新开箱，以服务端整窗读回结算。
// 步骤：复核库存与空量并认领委托 → 提交有界点击 → 重开同一箱核对净变化或保留待查。
async function transferItem(bot, withChest, source, args, job, claim, check, withdraw = false) {
  const result = job.receipt.result, signal = job.abort.signal;
  const field = withdraw ? 'withdrawal' : 'deposit';
  let stopped;
  // 桥接版本在刚采掘后的快捷栏点击内会延时。先等过这段时间，再绑定窗口并逐次即时复核。
  const settle = 500-(Date.now()-Number(bot.lastDigTime ?? 0));
  if (settle > 0) await delay(settle,undefined,{signal});
  await withChest(source,signal,async window => {
    const before = chestSample(bot,window,source.item);
    if (before.container.status !== 'sampled' || before.stock.status !== 'sampled' || window.selectedItem)
      throw chestError('chest_slots_unconfirmed','箱内、光标或伙伴库存尚未完整核实');
    check();
    claim();
    const limit = withdraw ? Math.min(args.count,before.container.stored,before.stock.knownSpace) :
      Math.min(args.count,before.stock.stored,before.container.knownSpace);
    const proof = result[field] = { work:job.id,observation:source.id,generation:source.generation,dimension:source.dimension,
      ...(withdraw && {direction:'withdraw',destination:args.destination}),
      owner:source.owner,actor:source.actor,cell:source.cell,pointedMessage:source.message,sourceMessage:args.sourceMessage,
      item:source.item,requested:args.count,limit,attempted:false,state:'pending',before };
    try {
      const click = checkedClicks(bot,window,proof,check);
      if (limit) stopped = withdraw ? await moveOutOfChest(window,proof,click) : await moveIntoChest(window,proof,click);
      if (!proof.attempted) result[field] = reconcileTransfer(proof,before);
    } catch (error) {
      if (!proof.attempted) result[field] = reconcileTransfer(proof,before);
      throw error;
    }
  });
  if (result[field].state === 'pending') {
    const after = await withChest(source,signal,window => chestSample(bot,window,source.item));
    result[field] = reconcileTransfer(result[field],after);
  }
  const proof = result[field];
  if (proof.state === 'pending') throw chestError('chest_effect_unconfirmed','两侧库存变化尚未核清，请查原工作');
  if (withdraw && proof.remaining) {
    const code = stopped ?? (proof.before.container.stored < args.count && proof.before.container.stored <= proof.before.stock.knownSpace
      ? 'chest_item_short' : !proof.before.stock.knownSpace ? 'chest_inventory_full' : 'chest_inventory_partial');
    throw chestError(code,'已核实取出 '+proof.withdrawn+'，剩余 '+proof.remaining);
  }
  if (proof.remaining) throw chestError(!proof.before.container.knownSpace ? 'chest_full' : 'chest_incomplete',
    '已核实存入 '+proof.stored+'，剩余 '+proof.remaining);
}

// 读取该原工作最近一次核对过的箱内转移凭据。
function chestProof(job) {
  return job?.latestChest ?? job?.receipt.result?.withdrawal ?? job?.receipt.result?.deposit;
}

module.exports = { chestOrder, transferItem, reconcileTransfer, chestProof };
