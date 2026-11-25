const { observeHandoff } = require('./handoff.cjs');
const { inventory, inventoryDelta } = require('./inventory-capacity.cjs');
const { handoffEvidence } = require('./handoff-evidence.cjs');
const { setTimeout: delay } = require('node:timers/promises');
const { once } = require('node:events');

// 保留交物失败的原因，供原工作区分条件变化和已提交的投物。
function blocked(code, message) { return Object.assign(new Error(message), { code }); }

// 在普通物品栏按选中槽位丢出指定总数，跨堆和部分栈逐次重验，绝不自动补投。
// 步骤：核对原观察、角色和收件人 → 朝向同一玩家 → 逐次提交生存丢物 → 保存退出时的库存读数。
async function tossHandoff(bot, owner, args, source, check, claim, result, signal) {
  const live = result.handoff = observeHandoff(bot, owner, args, source.id, source.generation);
  if (args.observation !== source.id || live.recipient !== source.recipient || live.item !== source.item || live.count !== source.count ||
      !live.actor.uuid || live.actor.uuid !== source.actor.uuid || live.dimension !== source.dimension)
    throw blocked('handoff_source_changed', '本次接收者、物品、数量、角色或维度与原交物观察不一致');
  const actor = bot.entity, before = inventory(bot);
  const transfer = result.transfer = { observation: source.id, recipient: owner, item: live.item, requested: live.count,
    submitted: 0, returnedClicks: 0, attemptedClicks: 0, stockBefore: before[live.item] ?? 0,
    sourceClaimed: false, progressAtMs: live.sampledAtMs };
  const recipient = live.target.state === 'visible' && bot.entities[live.target.entityId];

  // 封存实际阻止下一次投物的玩家位置、可见范围和当时尚未提交的数量。
  function recipientBlocked(sample, code, message) {
    result.recipientCheck = { observation: sample.id, generation: sample.generation, dimension: sample.dimension,
      recipient: sample.recipient, sampledAtMs: sample.sampledAtMs, remaining: sample.count,
      actor: sample.actor, target: sample.target, reasonCode: code };
    return blocked(code,message);
  }

  // 每次落键前复核同一角色、活的接收者、窗口和剩余库存。
  function ready(sample) {
    check();
    if (bot.entity !== actor || bot.game.dimension !== source.dimension) throw blocked('handoff_world_changed', '交物的原角色或维度已改变');
    if (actor.isValid === false || bot.health <= 0) throw blocked('handoff_actor_unavailable', '交物角色目前无法在场操作');
    if (!recipient || sample.target.state !== 'visible' || bot.entities[sample.target.entityId] !== recipient || !Number.isFinite(sample.target.distance))
      throw recipientBlocked(sample,'handoff_recipient_unavailable', '无法确认原接收者仍在当前交物现场的位置');
    if (sample.target.distance > 3)
      throw recipientBlocked(sample,'handoff_recipient_distant', '配对玩家尚未进入三格交物距离');
    if (sample.window !== 'inventory') throw blocked('handoff_window_busy', '交物需要普通物品栏且光标空闲');
    if (sample.stock.missing) throw blocked('handoff_stock_short', '可确认的普通库存不足以交出剩余数量');
  }
  try {
    ready(live);
    const aimed = recipient.position.clone();
    // 贴身时朝脚边投，给原地收件留出拾取范围；距离增加后逐渐抬高瞄准点。
    const aimHeight = Math.min(recipient.height ?? 1,1,Math.max(0,live.target.distance-1));
    await bot.lookAt(recipient.position.offset(0,aimHeight,0), true);
    // 强制转向先更新客户端，下一物理刻才发送朝向；缺区块或断线时这里仍可取消或到期。
    const timeout = AbortSignal.timeout(1500);
    try { await once(bot,'physicsTick',{ signal: signal ? AbortSignal.any([signal,timeout]) : timeout }); }
    catch { check(); throw blocked('handoff_facing_unconfirmed', '还没有能送出交物朝向的物理刻'); }
    while (transfer.submitted < live.count) {
      const remaining = live.count-transfer.submitted;
      const sample = observeHandoff(bot,owner,{ ...args, count: remaining },source.id,source.generation);
      ready(sample);
      if (recipient.position.distanceTo(aimed) > 0.75)
        throw recipientBlocked(sample,'handoff_recipient_moved', '接收者已离开本次瞄准的位置，剩余部分尚未投出');
      const row = sample.stock.selected[0];
      const whole = row.take === row.held;
      const quantity = whole ? row.held : 1;
      if (!transfer.sourceClaimed) { claim(); transfer.sourceClaimed = true; }
      ++transfer.attemptedClicks;
      transfer.submitted += quantity;
      transfer.progressAtMs = Math.floor(performance.now());
      // 丢整栈或单件均直接作用于选中槽，不把其余物品留在光标，也不切换快捷栏。
      await bot.clickWindow(row.slot, whole ? 1 : 0, 4);
      ++transfer.returnedClicks;
      transfer.progressAtMs = Math.floor(performance.now());
    }
  } finally {
    transfer.stockCurrent = bot.entity === actor && bot.game.dimension === source.dimension;
    if (transfer.stockCurrent) {
      const after = inventory(bot);
      transfer.stockAfter = after[live.item] ?? 0;
      result.inventoryDelta = inventoryDelta(before,after);
    }
    transfer.sampledAtMs = Math.floor(performance.now());
    transfer.progressAtMs = transfer.sampledAtMs;
  }
}

// 在同一服务端观察流中读回原工作的投物和去向；读断时保留此前已经核实的数量。
async function readHandoffProof(witness, evidence, last) {
  if (!evidence?.mark) return last;
  try {
    const rows = await witness.read(evidence.mark);
    const proof = handoffEvidence(rows,evidence.mark,evidence.actor,evidence.recipient,evidence.item,evidence.submitted,evidence.sourceThrough);
    if (last.source === 'server_events' && (proof.throughSeq < last.throughSeq || proof.thrown < last.thrown ||
        proof.recipientPicked < last.recipientPicked || proof.otherPicked < last.otherPicked)) return { ...last, coverage: 'interrupted' };
    return { ...proof, coverage: 'read' };
  } catch {
    return { ...last, coverage: 'interrupted' };
  }
}

// 判断已退出的交物是否仍需保留原票据，完整去向以已经返回的服务端证据为准。
function pendingHandoff(job) {
  const { skill, exited, result } = job.receipt;
  if (skill !== 'give_item' || !exited || !result.transfer?.submitted) return false;
  const proof = job.returnedProof ?? result.receipt;
  return proof?.source !== 'server_events' || proof.throwUnknown !== 0 || proof.pickupUnknown !== 0;
}

// 同一原工作上的显式观察和后台查证共同等待一次只读取样。
function refreshHandoff(job, witness) {
  job.proofRead ??= readHandoffProof(witness,job.handoffEvidence,job.latestProof ?? job.receipt.result.receipt)
    .then(proof => { job.latestProof = proof; job.proofAt = Math.floor(performance.now()); })
    .finally(() => { delete job.proofRead; });
  return job.proofRead;
}

// 从原工作封存的身份和采样结果构造回执，记录已经交给读者的证据。
function handoffSample(job, id) {
  const { generation, order, result } = job.receipt;
  const { observation, recipient, item, requested, submitted } = result.transfer;
  job.returnedProof = job.latestProof;
  return { id, work:job.id, order, observation, generation, dimension:result.handoff.dimension, recipient, item, requested, submitted,
    originalStatus:result.status, sampledAtMs:job.proofAt, sourceThrough:job.handoffEvidence?.sourceThrough ?? null, receipt:job.latestProof };
}

// 沿原号从封存的服务端流读取迟到收件，返回副本并保留动作退出时的原件。
// 步骤：核对原始取证条件 → 附上已读样本 → 对未决去向发起共享只读采样。
function originalHandoffReceipt(job, witness) {
  const { receipt } = job;
  if (receipt.skill !== 'give_item' || !receipt.exited || !job.handoffEvidence?.mark) return receipt;
  const sample = job.latestProof && handoffSample(job,job.id);
  if (pendingHandoff(job)) void refreshHandoff(job,witness);
  return sample ? { ...receipt, result:{ ...receipt.result, handoffReceipt:sample } } : receipt;
}

// 用可选只读观察端结算实际投出和接收者，普通服务器仍可完成丢物并说明未证数量。
// 步骤：固定取证窗口 → 执行原工作并保留中断 → 有界等待收件实效 → 封存投物来源上界。
async function giveItem(bot, owner, args, source, check, claim, job, witness) {
  const result = job.receipt.result;
  let mark, failure;
  if (witness) {
    try { mark = await witness.begin(source.dimension); } catch { /* 现场没有可读的观察流。 */ }
  }
  try { await tossHandoff(bot,owner,args,source,check,claim,result,job.abort?.signal); }
  catch (error) { failure = error; }
  const submitted = result.transfer?.submitted ?? 0;
  if (submitted) {
    const evidence = job.handoffEvidence = { mark, actor: source.actor.uuid, recipient: owner, item: source.item, submitted };
    result.receipt = { source: 'unavailable', coverage: mark ? 'interrupted' : 'unavailable', thrown: 0,
      recipientPicked: 0, otherPicked: 0, throwUnknown: submitted, pickupUnknown: 0 };
    const deadline = performance.now()+4000;
    if (mark) for (;;) {
      result.receipt = await readHandoffProof(witness,evidence,result.receipt);
      result.transfer.progressAtMs = Math.floor(performance.now());
      if (result.receipt.coverage !== 'read' || result.receipt.recipientPicked+result.receipt.otherPicked === submitted || failure) break;
      try { check(); } catch (error) { failure = error; break; }
      if (performance.now() >= deadline) break;
      await delay(80);
    }
    evidence.sourceThrough = result.receipt.throughSeq ?? mark?.seq;
  }
  if (failure) throw failure;
  if (result.receipt?.recipientPicked !== source.count) {
    if (result.receipt?.otherPicked) throw blocked('handoff_other_collector', '已有物品由其他实体收取，保留原玩家的实收量');
    throw blocked('handoff_receipt_unconfirmed', '丢物操作已经提交，原玩家的实收量尚未全部确认');
  }
}

module.exports = { tossHandoff, giveItem, readHandoffProof, pendingHandoff, refreshHandoff, handoffSample, originalHandoffReceipt };
