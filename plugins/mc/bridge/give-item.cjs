const { observeHandoff } = require('./handoff.cjs');
const { inventory, inventoryDelta } = require('./inventory-capacity.cjs');

// 保留交物失败的原因，供原工作区分条件变化和已提交的投物。
function blocked(code, message) { return Object.assign(new Error(message), { code }); }

// 在普通物品栏按选中槽位丢出指定总数，跨堆和部分栈逐次重验，绝不自动补投。
// 步骤：核对原观察、角色和收件人 → 朝向同一玩家 → 逐次提交生存丢物 → 保存退出时的库存读数。
async function tossHandoff(bot, owner, args, source, check, claim, result) {
  const live = result.handoff = observeHandoff(bot, owner, args, source.id, source.generation);
  if (args.observation !== source.id || live.recipient !== source.recipient || live.item !== source.item || live.count !== source.count ||
      !live.actor.uuid || live.actor.uuid !== source.actor.uuid || live.dimension !== source.dimension)
    throw blocked('handoff_source_changed', '本次接收者、物品、数量、角色或维度与原交物观察不一致');
  const actor = bot.entity, before = inventory(bot);
  const transfer = result.transfer = { observation: source.id, recipient: owner, item: live.item, requested: live.count,
    submitted: 0, returnedClicks: 0, attemptedClicks: 0, stockBefore: before[live.item] ?? 0, sourceClaimed: false };
  const recipient = live.target.state === 'visible' && bot.entities[live.target.entityId];

  // 每次落键前复核同一角色、活的接收者、窗口和剩余库存。
  function ready(sample) {
    check();
    if (bot.entity !== actor || bot.game.dimension !== source.dimension) throw blocked('handoff_world_changed', '交物的原角色或维度已改变');
    if (!recipient || sample.target.state !== 'visible' || bot.entities[sample.target.entityId] !== recipient)
      throw blocked('handoff_recipient_unavailable', '无法确认原接收者仍在当前视野');
    if (!Number.isFinite(sample.target.distance) || sample.target.distance > 3)
      throw blocked('handoff_recipient_distant', '配对玩家尚未进入三格交物距离');
    if (sample.window !== 'inventory') throw blocked('handoff_window_busy', '交物需要普通物品栏且光标空闲');
    if (sample.stock.missing) throw blocked('handoff_stock_short', '可确认的普通库存不足以交出剩余数量');
  }
  try {
    ready(live);
    await bot.lookAt(recipient.position.offset(0, Math.min(1,recipient.height ?? 1), 0), false);
    while (transfer.submitted < live.count) {
      const remaining = live.count-transfer.submitted;
      const sample = observeHandoff(bot,owner,{ ...args, count: remaining },source.id,source.generation);
      ready(sample);
      const row = sample.stock.selected[0];
      const whole = row.take === row.held;
      const quantity = whole ? row.held : 1;
      if (!transfer.sourceClaimed) { claim(); transfer.sourceClaimed = true; }
      ++transfer.attemptedClicks;
      transfer.submitted += quantity;
      // 丢整栈或单件均直接作用于选中槽，不把其余物品留在光标，也不切换快捷栏。
      await bot.clickWindow(row.slot, whole ? 1 : 0, 4);
      ++transfer.returnedClicks;
    }
  } finally {
    transfer.stockCurrent = bot.entity === actor && bot.game.dimension === source.dimension;
    if (transfer.stockCurrent) {
      const after = inventory(bot);
      transfer.stockAfter = after[live.item] ?? 0;
      result.inventoryDelta = inventoryDelta(before,after);
    }
    transfer.sampledAtMs = Math.floor(performance.now());
  }
}

module.exports = { tossHandoff };
