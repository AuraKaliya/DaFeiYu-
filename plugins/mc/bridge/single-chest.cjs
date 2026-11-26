const { once } = require('node:events');
const { setTimeout: delay } = require('node:timers/promises');
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
  let busy = false, pending;
  // 等原发送提交后取得同连接的服务端边界；失败后下次请求可以重新查证。
  async function recover() {
    const original = pending;
    if (!original) return;
    if (!original.checking) original.checking = original.sent.then(() => bot.tabComplete('/',false,false,1500))
      .then(original.release,() => {}).finally(() => { original.checking = null; });
    const timer = new AbortController();
    try { await Promise.race([original.checking,delay(1800,undefined,{signal:timer.signal})]); }
    finally { timer.abort(); }
    if (pending === original) throw chestError('chest_open_unsettled','原开箱尚未核实，可稍后重查服务端响应');
  }
  // 将晚到的应答归还给发起交互的控制器，换代与断线时释放监听。
  function drain(sent, owns) {
    const original = { sent };
    const release = () => {
      if (pending !== original) return;
      bot.off('windowOpen',discard);
      bot.off('end',release);
      bot.off('spawn',release);
      pending = undefined;
    };
    const discard = late => {
      if (owns() && bot.currentWindow === late && late.close) { late.close(); release(); }
    };
    original.release = release;
    pending = original;
    bot.on('windowOpen',discard);
    bot.once('end',release);
    bot.once('spawn',release);
    if (bot.currentWindow?.close) discard(bot.currentWindow);
    if (pending) void recover().catch(() => {});
  }
  // 只领取原控制器的应答，类型错误也归还该次实际打开的窗口。
  // 步骤：排空旧请求 → 复核原方块并激活 → 等待完整窗口 → 读取并归还。
  async function open(target, signal, use, owns = () => true) {
    if (pending) await recover();
    if (busy || bot.currentWindow || bot.inventory?.selectedItem || bot.getControlState('sneak'))
      throw chestError('chest_window_busy','伙伴还有打开的窗口、手持光标或潜行输入');
    const block = chestBlock(bot,target);
    signal.throwIfAborted();
    busy = true;
    let window, sent, submitted = false;
    const timeout = AbortSignal.timeout(2500);
    const activation = new AbortController();
    const wait = AbortSignal.any([signal,timeout,activation.signal]);
    const single = opened => ['minecraft:generic_9x3','minecraft:chest'].includes(opened.type) &&
      opened.inventoryStart === 27 && opened.inventoryEnd === 63;
    try {
      const opened = once(bot,'windowOpen',{signal:wait});
      sent = Promise.resolve().then(() => {
        wait.throwIfAborted();
        if (!owns()) throw chestError('chest_controller_changed','原控制器已经交还角色');
        submitted = true;
        return bot.activateBlock(block);
      }).catch(error => activation.abort(error));
      const [candidate] = await opened;
      if (!submitted || !owns() || bot.currentWindow !== candidate) throw chestError('chest_window_changed','窗口没有归属原控制器');
      window = candidate;
      if (!single(window)) throw chestError('chest_window_changed','服务端未给出所指单箱的完整窗口');
      chestBlock(bot,target);
      signal.throwIfAborted();
      return await use(window);
    } catch (error) {
      if (!window && submitted && wait.aborted && !activation.signal.aborted) {
        drain(sent,owns);
        throw chestError('chest_window_unconfirmed','未取得完整的服务端箱子窗口');
      }
      throw error;
    } finally {
      if (window && bot.currentWindow === window) window.close();
      busy = false;
    }
  }
  open.recover = recover;
  return open;
}

// 在同份服务端窗口中分别读箱子的 27 格及伙伴主背包与快捷栏的 36 格。
function chestSample(bot, window, item) {
  const size = bot.registry.itemsByName[item]?.stackSize;
  if (!size) throw chestError('chest_item_unknown','当前版本找不到指定物品');
  return { sampledAtMs: Math.floor(performance.now()),
    container: slotRange(window.slots,0,27,item,size), stock: slotRange(window.slots,27,63,item,size) };
}

module.exports = { chestError, chestWindows, chestSample };
