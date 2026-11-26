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

// 将明确存入的数量、物品和箱名与原观察相配，单独的选箱答复不能授权搬运。
function depositOrder(bot, source, pointing, instruction, aim, args) {
  if (Object.keys(args).length !== 4 || args.observation !== source.id || !Number.isSafeInteger(args.sourceMessage) ||
      args.sourceMessage < source.message || args.item !== source.item || !Number.isSafeInteger(args.count) || args.count < 1 || args.count > 2304)
    throw chestError('chest_order_mismatch','存入物品或数量与指定范围不符');
  const match = /^(?:请(?:你)?|麻烦(?:你)?)?(?:帮我)?(?:把|将)?(这|那)?([0-9一二两三四五六七八九十]+)(?:块|个)?([\p{L}\w:]*?)(?:存进|存入|放进|放入|放到|存到)(左箱|右箱|左边的箱子|右边的箱子|这个箱子|那个箱子|这个箱|那个箱)[。！!]?$/u
    .exec(instruction.replace(/\s+/g,''));
  const names = [source.item,'minecraft:'+source.item,bot.registry.itemsByName[source.item]?.displayName,
    ...(source.item === 'cobblestone' ? ['圆石','圓石'] : [])];
  if (!match || quantity(match[2]) !== args.count || (match[3] ? !names.includes(match[3]) : !match[1]))
    throw chestError('chest_explicit_order_required','需要当前玩家明确说出存入的物品或所指物料、数量和目标箱');
  const side = text => text.match(/(左|右)(?:边的箱子|箱)/)?.[1];
  const named = side(match[4]), selected = side(pointing);
  const aimed = aim && aim.dimension === source.dimension && ['x','y','z'].every(key => aim.cell[key] === source.cell[key]);
  if ((named && (selected !== named || source.message >= args.sourceMessage)) || (aim && !aimed) || (!named && !aimed))
    throw chestError('chest_target_mismatch','当前委托没有指向已核对的同一个箱子');
}

// 用两次完整服务端窗口核对双边数量，只把一致而且在提交范围内的净变化算作实存。
function reconcileDeposit(proof, after) {
  const amount = after.container.stored-proof.before.container.stored;
  if (after.container.status !== 'sampled' || after.stock.status !== 'sampled' || amount < 0 || amount > proof.limit ||
      amount !== proof.before.stock.stored-after.stock.stored) return proof;
  return { ...proof,state:'verified',after,stored:amount,remaining:proof.requested-amount };
}

// 只点击本次容器中的普通源栈，完整栈快速移动，超过剩余委托量的栈逐个放入。
// 步骤：核对当前窗口和原工作 → 按实有余量搬运 → 将手中余量放回原背包槽。
async function moveIntoChest(bot, window, proof, check) {
  let remaining = proof.limit;
  // 每次点击前复核窗口和普通栈覆盖，客户端预测只用于定位下一槽。
  const click = async (slot,button,mode) => {
    check();
    if (bot.currentWindow !== window) throw chestError('chest_window_closed','存放途中原窗口已关闭');
    const sample = chestSample(bot,window,proof.item);
    if (sample.container.status !== 'sampled' || sample.stock.status !== 'sampled')
      throw chestError('chest_slots_changed','点击前槽位覆盖或普通物品栈已变化');
    proof.attempted = true;
    await bot.clickWindow(slot,button,mode);
    if (bot.currentWindow !== window) throw chestError('chest_window_closed','存放途中原窗口已关闭');
  };
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

// 固定原工作的双边基线，转移后重新开箱，以服务端整窗读回结算。
// 步骤：复核库存与空量并认领委托 → 提交有界点击 → 重开同一箱核对净变化或保留待查。
async function depositItem(bot, withChest, source, args, job, claim, check) {
  const result = job.receipt.result, signal = job.abort.signal;
  // 桥接版本在刚采掘后的快捷栏点击内会延时。先等过这段时间，再绑定窗口并逐次即时复核。
  const settle = 500-(Date.now()-Number(bot.lastDigTime ?? 0));
  if (settle > 0) await delay(settle,undefined,{signal});
  await withChest(source,signal,async window => {
    const before = chestSample(bot,window,source.item);
    if (before.container.status !== 'sampled' || before.stock.status !== 'sampled' || window.selectedItem)
      throw chestError('chest_slots_unconfirmed','箱内、光标或伙伴库存尚未完整核实');
    check();
    claim();
    const limit = Math.min(args.count,before.stock.stored,before.container.knownSpace);
    const proof = result.deposit = { work:job.id,observation:source.id,generation:source.generation,dimension:source.dimension,
      owner:source.owner,actor:source.actor,cell:source.cell,pointedMessage:source.message,sourceMessage:args.sourceMessage,
      item:source.item,requested:args.count,limit,attempted:false,state:'pending',before };
    try {
      if (limit) await moveIntoChest(bot,window,proof,check);
      else result.deposit = reconcileDeposit(proof,before);
    } catch (error) {
      if (!proof.attempted) result.deposit = reconcileDeposit(proof,before);
      throw error;
    }
  });
  if (result.deposit.state === 'pending') {
    const after = await withChest(source,signal,window => chestSample(bot,window,source.item));
    result.deposit = reconcileDeposit(result.deposit,after);
  }
  const proof = result.deposit;
  if (proof.state === 'pending') throw chestError('chest_effect_unconfirmed','两侧库存变化尚未核清，请查原工作');
  if (proof.remaining) throw chestError(!proof.before.container.knownSpace ? 'chest_full' : 'chest_incomplete',
    '已核实存入 '+proof.stored+'，剩余 '+proof.remaining);
}

module.exports = { depositOrder, depositItem, reconcileDeposit };
