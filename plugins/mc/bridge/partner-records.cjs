const { chestProof } = require('./chest-transfer.cjs');
const { pendingHandoff } = require('./give-item.cjs');

// 为下一笔游戏工作留出回执位置，将可复查的单箱转移基线和未决交物保留给原工作。
// 步骤：保护本次引用和未决实效 → 按原消息边界回收已结算工作 → 满额时接纳原号查证。
function makeReceiptRoom(records, active, context, lookup, inPresence = () => true, withdrawal) {
  while (records.size >= 64) {
    const settled = [...records].find(([, job]) => job !== active && job.receipt.exited && !pendingHandoff(job) &&
      job !== lookup && job !== withdrawal && (chestProof(job)?.state !== 'pending' ||
        job.receipt.generation !== context.generation || job.receipt.controller !== context.controller || !inPresence(job.receipt.generation,job.receipt.order)) &&
      (job.receipt.generation !== context.generation || job.receipt.order !== context.order || job.receipt.controller !== context.controller));
    if (!settled) {
      if (records.size === 64 && ['give_item','store_item','withdraw_item'].includes(lookup?.receipt.skill)) return;
      throw new Error('游戏工作回执已满');
    }
    records.delete(settled[0]);
  }
}

module.exports = { makeReceiptRoom };
