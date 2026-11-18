// 在已接单的地标区域内执行有界单块采掘，保留原编号的结果。
// 步骤：查回世界与授权 → 复核可见方块和工具 → 执行或停止 → 保存客户端事实。
function miningService(bot, world, deliveries, places, inspection) {
  let batch = null, active = null, done = Promise.resolve(), timer;
  let lastPoll = 0, closed = false;
  let revoked = null;

  // 只接受当前世界和已接单的原命令。
  function grantFor(generation, question, grant) {
    if (closed || world.read()?.generation !== generation ||
      (revoked?.generation === generation && revoked.grant === grant)) return null;
    const receipt = deliveries.read(generation, question, grant);
    return receipt?.phase === 'accepted' ? receipt : null;
  }
  // 停止当前真实挖掘，退出由异步调用归还后记录。
  function stop() {
    if (!active || active.exited) return;
    active.stopping = true;
    if (bot.targetDigBlock) bot.stopDigging();
  }
  // 只在查看过的地标附近选择角色当前看得见、够得着的约定方块。
  function target(grant) {
    const seen = inspection(grant.generation, grant.order);
    const place = places.get(seen?.place);
    if (!place || seen.phase !== 'arrived' || !seen.stone || place.dimension !== bot.game.dimension)
      throw new Error('原始发现不可用');
    const positions = bot.findBlocks({ matching: bot.registry.blocksByName.cobblestone.id, maxDistance: 6, count: 64 });
    for (const position of positions) {
      const block = bot.blockAt(position);
      if (Math.hypot(position.x - place.x, position.y - place.y, position.z - place.z) <= 8 &&
        block?.name === 'cobblestone' && bot.canSeeBlock(block) && bot.canDigBlock(block)) return block;
    }
    throw new Error('没有可达的可见圆石');
  }
  // 装备后再次核对授权与同一对象，客户端完成不作为拾取或实际带回的证明。
  async function execute(action, grant) {
    try {
      const chosen = target(grant);
      const pick = bot.inventory.items().find(item => item.name.endsWith('_pickaxe') && chosen.canHarvest(item.type));
      if (!pick) throw new Error('缺少合适的工具');
      await bot.equip(pick, 'hand');
      const current = bot.blockAt(chosen.position);
      if (action.stopping || !grantFor(grant.generation, grant.question, grant.grant) ||
        current?.type !== chosen.type || !bot.canSeeBlock(current) || !bot.canDigBlock(current)) throw new Error('目标变化');
      action.target = { x: current.position.x, y: current.position.y, z: current.position.z };
      action.started = true;
      action.phase = 'digging';
      await bot.dig(current);
      action.phase = action.stopping ? 'stopped' : 'clientFinished';
    } catch {
      action.phase = action.stopping ? 'stopped' : 'failed';
    } finally {
      action.exited = true;
      clearInterval(timer);
    }
  }
  bot.on('respawn', stop);
  bot.on('end', stop);
  return {
    // 每个授权只能依次接纳配额内的新编号；重复编号复用原事实。
    begin(generation, question, id, index) {
      const grant = grantFor(generation, question, id);
      if (!grant || !Number.isSafeInteger(index) || index < 1 || index > grant.quantity) throw new Error('采集超出原授权');
      if (batch?.generation !== generation || batch.grant !== id) {
        if (active && !active.exited) throw new Error('旧采掘仍在执行');
        batch = { generation, grant: id, records: [] };
      }
      if (index <= batch.records.length) return { ...batch.records[index - 1] };
      if (index !== batch.records.length + 1 || (batch.records.length && active.phase !== 'clientFinished'))
        throw new Error('采掘前序尚未完成');
      active = { generation, question, grant: id, index, phase: 'preparing', started: false, exited: false, stopping: false, target: null };
      batch.records.push(active);
      lastPoll = performance.now();
      const deadline = lastPoll + 20000;
      timer = setInterval(() => {
        if (performance.now() >= deadline || performance.now() - lastPoll > 4000 ||
          world.read()?.generation !== generation) stop();
      }, 200);
      timer.unref();
      done = execute(active, grant);
      return { ...active };
    },
    // 按原编号读回或停止，不重启迟到动作。
    poll(generation, question, id, index, cancel) {
      const value = batch?.records[index - 1];
      if (!value || value.generation !== generation || value.grant !== id || value.question !== question)
        throw new Error('采掘编号失效');
      if (value === active) {
        lastPoll = performance.now();
        if (cancel) stop();
      }
      return { ...value };
    },
    // 撤销当前已接单范围，在途工作协作停止，旧授权不能再开始下一块。
    revoke(generation, question, grant) {
      const value = deliveries.read(generation, question, grant);
      if (value?.phase !== 'accepted') throw new Error('撤销目标不是当前委托');
      revoked = { generation, grant };
      if (active?.grant === grant && active.generation === generation) stop();
      return { generation, question, grant, revoked: true, exited: !active || active.exited };
    },
    // 禁止新动作并等到当前挖掘调用真正归还。
    async close() {
      closed = true;
      stop();
      bot.removeListener('respawn', stop);
      bot.removeListener('end', stop);
      await done;
    },
  };
}
module.exports = { miningService };
