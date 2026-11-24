const { pathfinder, Movements, goals } = require('mineflayer-pathfinder');
const { collectionDesk } = require('./delivery.cjs');
const { miningService } = require('./mining.cjs');
const { returnService } = require('./returning.cjs');
const { exploration, sampleView } = require('./exploration.cjs');
const { partnerService } = require('./partner.cjs');

// 将用户设置里的语义地标复制为有限的游戏坐标，不接受模型给出的任意落点。
function landmarks(input = {}) {
  if (!input || typeof input !== 'object' || Array.isArray(input) || Object.keys(input).length > 16)
    throw new Error('无效地标目录');
  const result = new Map();
  for (const [id, place] of Object.entries(input)) {
    if (!/^[a-z][a-z0-9-]{0,23}$/.test(id) || !place || typeof place.label !== 'string' || !place.label.trim() ||
      place.label.length > 32 || typeof place.dimension !== 'string' || !place.dimension || place.dimension.length > 128 ||
      ![place.x, place.y, place.z].every(Number.isSafeInteger) || Math.abs(place.x) > 30000000 ||
      Math.abs(place.z) > 30000000 || Math.abs(place.y) > 2048) throw new Error('无效地标');
    if ([...result.values()].some(entry => entry.label === place.label)) throw new Error('地标名称重复');
    result.set(id, { label: place.label, dimension: place.dimension, x: place.x, y: place.y, z: place.z });
  }
  return result;
}

// 当前首例只从玩家明确要求前往该地标查看的语句授予移动能力。
function permitsInspection(text, label) {
  const natural = text.trim().replace(/[。！？!?]$/u, '');
  return [`去${label}看看`, `请去${label}看看`, `帮我去${label}看看`].includes(natural);
}

// 只为可精确绑定的查看原话提供单一地标候选。
function inspectionOffer(text, places) {
  for (const [place, info] of places) if (permitsInspection(text, info.label)) return { purpose: 'visit.inspect', place };
  return null;
}

// 只记录角色从到达点真实可见的圆石位置。
function visibleStone(bot) {
  const positions = bot.findBlocks({ matching: bot.registry.blocksByName.cobblestone.id, maxDistance: 8, count: 128 });
  for (const position of positions) {
    const block = bot.blockAt(position);
    if (block?.name === 'cobblestone' && bot.canSeeBlock(block)) return { x: position.x, y: position.y, z: position.z };
  }
  return null;
}

// 提供绑定到原世界和玩家消息的一次查看技能，路线执行交给游戏侧。
// 步骤：固定零采掘的寻路策略 → 接纳或复用原动作 → 读回位置与视线 → 在撤销时停止并等待退出。
function inspectionService(bot, world, inbox, places, witness = null, modelLed = false, owner = null) {
  bot.loadPlugin(pathfinder);
  let current = null;
  let timer = null;
  let done = Promise.resolve();
  let closed = false;
  let heartbeat = 0;
  let origin = null;
  let survey = null;
  let routeState = '正在计算路径';
  let resetReason = '';
  // 只引用寻路器实际报告的状态，路径计划与实地观察各自留证。
  function pathUpdate(result) {
    if (current?.phase !== 'moving') return;
    routeState = { success: '已规划可行路径', partial: '仍在计算后续路径', timeout: '本次寻路计算超时',
      noPath: '本次寻路没有找到可行路径' }[result.status] ?? '等待新的路径状态';
  }
  // 保存当前动作最近一次可辨认的重算原因，普通位置回读不生成原因。
  function pathReset(reason) {
    if (current?.phase !== 'moving') return;
    resetReason = { stuck: '移动停滞', block_updated: '方块发生变化', chunk_loaded: '新区块已载入',
      goal_moved: '目标位置变化' }[reason] ?? '';
  }
  // 仅在本次地标查看的移动阶段收集角色可见事实。
  function observe() {
    if (survey && current?.phase === 'moving' && !current.exited) sampleView(bot, survey, performance.now());
  }
  const gap = () => survey?.reset();
  let question = null;
  let answered = false;
  let release = null;
  const deliveries = collectionDesk(world, inbox, () => question, () => !closed);
  const mining = miningService(bot, world, deliveries, places, (generation, order) =>
    current?.generation === generation && current.order === order ? current : null, witness);
  const returning = returnService(bot, world, deliveries, mining, places, witness);
  const partner = modelLed ? partnerService(bot, world, inbox, places, owner, witness) : null;
  // 在当前路径尚未退出时保存实际观察到的位移。
  function moved() {
    if (current && !current.exited && origin && bot.entity.position.distanceTo(origin) > 0.05) current.moved = true;
  }

  // 停止本观察动作的路径和控制输入，实际退出由原工作处理。
  function stop() {
    if (!current || current.exited) return;
    current.phase = 'stopping';
    bot.pathfinder.setGoal(null);
    bot.clearControlStates();
  }
  // 推进路径到达并在同代次下重新检查可见状态。
  async function walk(action, place) {
    try {
      const moves = new Movements(bot);
      moves.canDig = moves.canOpenDoors = moves.allow1by1towers = moves.allowParkour = false;
      moves.scafoldingBlocks = [];
      bot.pathfinder.setMovements(moves);
      await bot.pathfinder.goto(new goals.GoalNear(place.x, place.y, place.z, 1));
      const snapshot = world.read();
      if (closed || action.phase !== 'moving' || snapshot?.generation !== action.generation) throw new Error('revoked');
      const pos = bot.entity.position.floored();
      if (Math.hypot(pos.x - place.x, pos.y - place.y, pos.z - place.z) > 1) throw new Error('arrival mismatch');
      action.position = snapshot.position;
      action.stone = visibleStone(bot);
      action.phase = 'arrived';
    } catch {
      action.phase = action.phase === 'stopping' ? 'stopped' : 'failed';
    } finally {
      clearInterval(timer);
      bot.pathfinder.setGoal(null);
      bot.clearControlStates();
      action.exited = true;
    }
  }
  bot.on('respawn', stop);
  bot.on('end', stop);
  bot.on('move', moved);
  bot.on('physicsTick', observe);
  bot.on('forcedMove', gap);
  bot.on('path_update', pathUpdate);
  bot.on('path_reset', pathReset);
  // 先封闭全部动作入口，再等原寻路、采掘和返程的异步调用实际返回。
  // 步骤：撤销接纳 → 停止游戏输入 → 解除监听并等待各工作退出。
  async function close() {
    closed = true;
    stop();
    bot.removeListener('respawn', stop);
    bot.removeListener('end', stop);
    bot.removeListener('move', moved);
    bot.removeListener('physicsTick', observe);
    bot.removeListener('forcedMove', gap);
    bot.removeListener('path_update', pathUpdate);
    bot.removeListener('path_reset', pathReset);
    await Promise.all([done, mining.close(), returning.close(), partner?.close()]);
  }
  // 一次接管只关联原游戏代次，重投不覆盖首个来源或再次停止。
  function releaseControl(generation, order = 0) {
    if (world.read()?.generation !== generation || (release && release.generation !== generation))
      throw new Error('接管的游戏绑定已失效');
    if (!release) {
      const record = { generation, order, phase: 'requested' };
      release = record;
      close().then(() => { record.phase = 'released'; }, () => { record.phase = 'unconfirmed'; });
    }
    return { ...release };
  }
  return {
    partner,
    releaseControl,
    // 私有收件同时返回原接管回执，普通状态和其他世界不暴露这项玩家决定。
    controlReceipt(generation) { return release?.generation === generation ? { ...release } : null; },
    // 报告本连接的执行入口，角色重生不会撤回已经关闭的控制。
    execution() { return { mode: partner ? 'partner' : 'scenario', state: release?.phase ?? (closed ? 'unavailable' : 'ready') }; },
    // 只返回同一游戏来源和原查看消息的观察。
    progress(generation) {
      return current && generation === current.generation && current.phase === 'moving' ? survey?.read(performance.now()) ?? null : null;
    },
    // 直接回答当前动作的路线询问，不中断移动或占用另一次模型调用。
    explain(generation, order, inquiry) {
      if (!current || current.generation !== generation || current.order !== order) return null;
      const label = places.get(current.place).label;
      const destination = current.phase === 'moving' ? `我正在前往${label}。` : `前往${label}的动作已离开移动阶段（${current.phase}）。`;
      return { order, inquiry, text: destination + routeState + '。' + (resetReason ? `上次重算路径的原因是${resetReason}。` : '') };
    },
    deliveries,
    mining,
    returning,
    // 精确复用同一委托，检查原文和目标后才允许开始一次移动。
    begin(generation, order, placeId) {
      const snapshot = world.read();
      if (closed || !snapshot || snapshot.generation !== generation || !Number.isSafeInteger(order) || order < 1)
        throw new Error('旧世界或无效来源');
      if (current?.generation === generation && current.order === order) {
        if (current.place !== placeId) throw new Error('查看目标冲突');
        return { ...current };
      }
      if (current && (!current.exited || (current.generation === generation && order <= current.order))) throw new Error('查看占用或过期');
      const place = places.get(placeId);
      const entry = inbox.get(generation, order);
      if (!place || snapshot.dimension !== place.dimension || !entry || !permitsInspection(entry.text, place.label))
        throw new Error('没有此地标的查看委托');
      origin = bot.entity.position.clone();
      survey = exploration(order);
      routeState = '正在计算路径';
      resetReason = '';
      question = null;
      answered = false;
      current = { generation, order, place: placeId, phase: 'moving', exited: false, moved: false };
      heartbeat = performance.now();
      const deadline = heartbeat + 30000;
      timer = setInterval(() => {
        const now = performance.now();
        if (now > deadline || now - heartbeat > 4000 || world.read()?.generation !== generation) stop();
      }, 200);
      timer.unref();
      done = walk(current, place);
      return { ...current };
    },
    // 查询或停止原动作；其他编号不能取得或改变当前路径。
    poll(generation, order, cancel) {
      if (!current || current.generation !== generation || current.order !== order) throw new Error('查看动作已失效');
      heartbeat = performance.now();
      if (cancel) stop();
      return { ...current };
    },
    // 发现目标后向同场的绑定玩家私聊提问，同一问题重投只返回原发送回执。
    // 步骤：复核原查看证据 → 找到绑定玩家 → 保存问题身份并发送一次。
    ask(generation, order, id) {
      if (closed || !/^[a-zA-Z0-9.-]{1,96}$/.test(id) || world.read()?.generation !== generation ||
        current?.generation !== generation || current.order !== order || current.phase !== 'arrived' ||
        !current.exited || !current.stone) throw new Error('没有可提问的观察');
      if (question) {
        if (question.id !== id || !question.sent) throw new Error('原问题冲突或发送未核实');
        return { ...question };
      }
      const source = inbox.read(generation, 0);
      const player = Object.values(bot.players).find(value => value.uuid === source?.owner);
      if (!player || !/^[a-zA-Z0-9_]{1,16}$/.test(player.username)) throw new Error('原玩家不在场');
      const text = '我在' + places.get(current.place).label + '看到了圆石。要我采一些带回来吗？';
      const meeting = places.get('riverside');
      question = { id, generation, order, owner: source.owner, after: source.latest, text, sent: false,
        material: 'minecraft:cobblestone', returnPlace: meeting?.dimension === world.read()?.dimension ? 'riverside' : null };
      bot.whisper(player.username, text);
      question.sent = true;
      return { ...question };
    },
    // 仅在原问题已发出后识别绑定玩家明确的拒绝，其余聊天保持普通消息。
    answerOffer(text) {
      if (!question?.sent || answered || world.read()?.generation !== question.generation) return null;
      const answer = text.trim().replace(/[。！!]$/u, '');
      if (['不用了', '不要采', '先别采了'].includes(answer))
        return { purpose: 'question.decline', question: question.id };
      const match = /^(?:采|请采|帮我采)\s*([1-9][0-9]?)\s*块(?:圆石)?带回来$/u.exec(answer);
      if (!match || Number(match[1]) > 64 || !question.returnPlace) return null;
      return { purpose: 'collect.return', question: question.id, quantity: Number(match[1]) };
    },
    // 关闭入口并等待本次路径结束，移除游戏事件监听。
    close,
  };
}
module.exports = { inspectionService, landmarks, permitsInspection, inspectionOffer };
