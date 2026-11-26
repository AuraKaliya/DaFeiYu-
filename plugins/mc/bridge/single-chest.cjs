const { once } = require('node:events');
const { slotRange } = require('./inventory-capacity.cjs');

// 生成可由原工作回执区分的单箱受阻原因。
function chestError(code, message) {
  return Object.assign(new Error(message), { code });
}

// 从冻结的指向复核当前格位、普通单箱类型以及伙伴可交互的距离。
function chestBlock(bot, target) {
  if (!target || target.dimension !== bot.game.dimension)
    throw chestError('chest_not_pointed','原话没有在当前维度指中一个普通单箱');
  const { x, y, z } = target.cell;
  const pos = bot.entity.position.clone().set(x,y,z);
  const block = bot.blockAt(pos);
  if (block?.name !== 'chest' || block.getProperties().type !== 'single')
    throw chestError('chest_changed','原格位当前无法核实为普通单箱');
  if (pos.offset(0.5,0.5,0.5).distanceTo(bot.entity.position.offset(0,bot.entity.eyeHeight ?? 1.62,0)) > 4.5 ||
      !bot.canSeeBlock(block)) throw chestError('chest_out_of_reach','伙伴当前位置不能触及所指单箱');
  return block;
}

// 一次只打开一个已核实的箱子，并且只关闭本次取得的窗口。
// 步骤：复核现场 → 等服务端同步窗口 → 交给调用者读取 → 回收原窗口或排空迟到响应。
function chestWindows(bot) {
  let busy = false;
  return async (target, signal, use) => {
    if (busy || bot.currentWindow || bot.inventory?.selectedItem || bot.getControlState('sneak'))
      throw chestError('chest_window_busy','伙伴还有打开的窗口、手持光标或潜行输入');
    const block = chestBlock(bot,target);
    signal.throwIfAborted();
    busy = true;
    let window, sent, draining = false;
    const timeout = AbortSignal.timeout(2500);
    const activation = new AbortController();
    const wait = AbortSignal.any([signal,timeout,activation.signal]);
    const single = opened => ['minecraft:generic_9x3','minecraft:chest'].includes(opened.type) &&
      opened.inventoryStart === 27 && opened.inventoryEnd === 63;
    try {
      const opened = once(bot,'windowOpen',{signal:wait});
      sent = Promise.resolve().then(() => bot.activateBlock(block)).catch(error => activation.abort(error));
      const [candidate] = await opened;
      if (bot.currentWindow !== candidate || !single(candidate))
        throw chestError('chest_window_changed','服务端未给出所指单箱的完整窗口');
      window = candidate;
      chestBlock(bot,target);
      signal.throwIfAborted();
      return await use(window);
    } catch (error) {
      if (!window && wait.aborted && !activation.signal.aborted) {
        draining = true;
        // 同一连接的补全响应给出开箱请求之后的服务端边界，期间迟到的窗口立即归还。
        const discard = late => { if (bot.currentWindow === late && single(late)) late.close(); };
        const reset = () => { bot.off('windowOpen',discard); bot.off('end',reset); busy = false; };
        bot.on('windowOpen',discard);
        bot.once('end',reset);
        if (bot.currentWindow?.close) discard(bot.currentWindow);
        sent.then(() => bot.tabComplete('/help ',false,false,3000)).then(reset,() => {});
        throw chestError('chest_window_unconfirmed','未取得完整的服务端箱子窗口');
      }
      throw error;
    } finally {
      if (window && bot.currentWindow === window) window.close();
      if (!draining) busy = false;
    }
  };
}

// 在同份服务端窗口中分别读箱子的 27 格及伙伴主背包与快捷栏的 36 格。
function chestSample(bot, window, item) {
  const size = bot.registry.itemsByName[item]?.stackSize;
  if (!size) throw chestError('chest_item_unknown','当前版本找不到指定物品');
  return { sampledAtMs: Math.floor(performance.now()),
    container: slotRange(window.slots,0,27,item,size), stock: slotRange(window.slots,27,63,item,size) };
}

module.exports = { chestError, chestWindows, chestSample };
