const { Movements, goals } = require('mineflayer-pathfinder');
const { setTimeout: delay } = require('node:timers/promises');

// 用抵达后的服务器联合采样及原拾取证据计算仍持有的自采量。
// 步骤：定位同一角色的新到达样本 → 核对每个来源 → 用途中材料减少及库存上限收紧范围。
function carriedAt(rows, records, actor, destination, departure, client) {
  const arrival = rows.findLast(row => row.kind === 'positionStock' && row.actor === actor &&
    row.seq > departure.seq && row.stream === departure.stream && row.stock === client.cobblestone &&
    Math.hypot(row.x - destination.x - 0.5, row.y - destination.y, row.z - destination.z - 0.5) <= 1.8 &&
    Math.hypot(row.x - client.position.x, row.y - client.position.y, row.z - client.position.z) <= 1.5);
  if (!arrival) return null;
  const proofs = records.map(record => record.pickup);
  if (proofs.some(proof => !proof || proof.stream !== arrival.stream || proof.last >= arrival.seq)) return null;
  if (new Set(proofs.map(proof => proof.first)).size !== proofs.length) return null;
  let lower = proofs.reduce((sum, proof) => sum + proof.lower, 0);
  let upper = Math.min(arrival.stock, proofs.reduce((sum, proof) => sum + proof.upper, 0));
  for (const row of rows) {
    if (row.seq > arrival.seq || row.actor !== actor) continue;
    if (row.kind === 'stoneDebit') lower -= row.amount;
    if (row.kind === 'positionStock') {
      const later = proofs.filter(proof => proof.last > row.seq).reduce((sum, proof) => sum + proof.upper, 0);
      upper = Math.min(upper, row.stock + later);
    }
  }
  return { lower: Math.max(0, Math.min(lower, upper)), upper, stream: arrival.stream, sample: arrival.seq, stock: arrival.stock };
}

// 只允许已完成配额内采掘的原委托返回其登记会合区，持有量另取联合读回。
function returnService(bot, world, deliveries, mining, places, witness) {
  let current = null, done = Promise.resolve(), timer, lastPoll = 0, closed = false;
  // 撤销原路径并等执行流程自行回收。
  function stop() {
    if (!current || current.exited) return;
    current.phase = 'stopping';
    bot.pathfinder.setGoal(null);
    bot.clearControlStates();
  }
  // 按游戏自身可见状态步行，达到目的地后读取新观察。
  async function walk(action, destination, summary) {
    try {
      let departure = null;
      if (witness && summary.mark) try { departure = await witness.begin(bot.game.dimension); } catch { }
      if (closed || action.phase === 'stopping' || world.read()?.generation !== action.generation) throw new Error('失效');
      const moves = new Movements(bot);
      moves.canDig = moves.canOpenDoors = moves.allow1by1towers = moves.allowParkour = false;
      moves.scafoldingBlocks = [];
      bot.pathfinder.setMovements(moves);
      await bot.pathfinder.goto(new goals.GoalNear(destination.x, destination.y, destination.z, 1));
      const until = performance.now() + 1500;
      do {
        const sample = world.read();
        if (closed || action.phase === 'stopping' || sample?.generation !== action.generation ||
          sample.dimension !== destination.dimension || Math.hypot(sample.position.x - destination.x - 0.5,
            sample.position.y - destination.y, sample.position.z - destination.z - 0.5) > 1.8) throw new Error('到达未证实');
        action.position = sample.position;
        action.stock = sample.cobblestone;
        if (!departure) break;
        try { action.carried = carriedAt(await witness.read(summary.mark), summary.records, sample.actor, destination, departure, sample); }
        catch { break; }
        if (action.carried) break;
        await delay(60);
      } while (performance.now() < until);
      if (closed || action.phase === 'stopping' || world.read()?.generation !== action.generation) throw new Error('返程已撤销');
      action.phase = 'arrived';
    } catch {
      action.phase = action.phase === 'stopping' ? 'stopped' : 'failed';
    } finally {
      clearInterval(timer);
      bot.clearControlStates();
      action.exited = true;
    }
  }
  bot.on('respawn', stop);
  bot.on('end', stop);
  return {
    // 复核原委托和最后一块实际退出，重复请求返回同一趟返程。
    begin(generation, question, grant) {
      const receipt = deliveries.read(generation, question, grant);
      if (closed || receipt?.phase !== 'accepted' || world.read()?.generation !== generation) throw new Error('返程授权失效');
      if (current?.generation === generation && current.grant === grant) return { ...current };
      const summary = mining.summary(generation, question, grant);
      const destination = places.get(receipt.returnPlace);
      if ((current && !current.exited) || !destination || world.read().dimension !== destination.dimension ||
          summary?.records.length !== receipt.quantity || summary.records.some(r => !r.exited || r.phase !== 'clientFinished'))
        throw new Error('返程前序不完整');
      current = { generation, question, grant, place: receipt.returnPlace, phase: 'returning', exited: false,
        stock: null, position: null, carried: null };
      lastPoll = performance.now();
      const deadline = lastPoll + 30000;
      timer = setInterval(() => { if (performance.now() - lastPoll > 4000 || performance.now() >= deadline) stop(); }, 200);
      timer.unref();
      done = walk(current, destination, summary);
      return { ...current };
    },
    // 查询或停止同一趟返程，未取得到达采样不能显示已交付。
    poll(generation, question, grant, cancel) {
      if (!current || current.generation !== generation || current.question !== question || current.grant !== grant)
        throw new Error('返程身份失效');
      lastPoll = performance.now();
      if (cancel) stop();
      return { ...current };
    },
    // 等待本次移动退出并解除事件监听。
    async close() {
      closed = true; stop(); bot.removeListener('respawn', stop); bot.removeListener('end', stop); await done;
    },
  };
}
module.exports = { returnService, carriedAt };
