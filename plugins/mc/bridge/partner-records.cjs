const { pendingHandoff } = require('./give-item.cjs');

// 为下一笔游戏工作腾出原号回执位置，已投出但去向待查的记录继续持有。
// 步骤：按原消息边界回收已结算工作 → 满额时只容纳一个查证已有交物的读口。
function makeReceiptRoom(records, active, context, lookup) {
  while (records.size >= 64) {
    const settled = [...records].find(([, job]) => job !== active && job.receipt.exited && !pendingHandoff(job) &&
      (job.receipt.generation !== context.generation || job.receipt.order !== context.order || job.receipt.controller !== context.controller));
    if (!settled) {
      if (records.size === 64 && lookup?.receipt.skill === 'give_item') return;
      throw new Error('游戏工作回执已满');
    }
    records.delete(settled[0]);
  }
}

module.exports = { makeReceiptRoom };
