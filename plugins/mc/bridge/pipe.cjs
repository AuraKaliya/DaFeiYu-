const net = require('node:net');
const { once } = require('node:events');
const { timingSafeEqual } = require('node:crypto');

// 从本机通道短名构造固定命名空间，禁止传入路径。
function pipeName(channel) {
  if (typeof channel !== 'string' || !/^[a-zA-Z0-9-]{1,48}$/.test(channel)) throw new Error('invalid MC channel');
  return '\\\\.\\pipe\\DaFeiYu.MC.' + channel;
}

// 处理一份有界状态查询，编号只用于关联，不承担授权含义。
function readReply(input, world, inbox, key, actions) {
  const request = JSON.parse(input);
  if (!request || request.version !== 1 || typeof request.id !== 'string' ||
    !/^[0-9a-zA-Z.-]{1,96}$/.test(request.id)) throw new Error('invalid MC request');
  if (request.method === 'state.read' && Object.keys(request).length === 3)
    return { version: 1, id: request.id, state: world.read() };
  if (!key || typeof request.key !== 'string' || !/^[a-f0-9]{64}$/.test(request.key) ||
    !timingSafeEqual(Buffer.from(request.key), Buffer.from(key))) throw new Error('invalid MC pairing');
  const explain = request.method === 'survey.read' && Object.keys(request).length === 6 &&
    Number.isSafeInteger(request.order) && request.order > 0 && typeof request.inquiry === 'string' && /^[1-9][0-9]{0,19}$/.test(request.inquiry);
  if (request.method === 'survey.read' && (Object.keys(request).length === 4 || explain)) {
    const state = world.read();
    const survey = state && actions?.progress?.(state.generation);
    const answer = state && explain && actions?.explain?.(state.generation, request.order, request.inquiry);
    return { version: 1, id: request.id, state: state ? { ...state, ...(survey && { survey }), ...(answer && { answer }) } : null };
  }
  const controller = typeof request.controller === 'string' && /^[a-f0-9]{32}$/.test(request.controller);
  const listening = request.method === 'partner.listen' && Object.keys(request).length === 7 && controller;
  if ((listening || (request.method === 'chat.read' && Object.keys(request).length === 6)) && inbox &&
    typeof request.generation === 'string' && Number.isSafeInteger(request.after) && request.after >= 0) {
    const batch = inbox.read(request.generation, request.after);
    const control = actions?.controlReceipt?.(request.generation);
    const sceneRevision = batch && actions?.partner?.scene?.(request.generation);
    let execution = actions?.execution?.() ?? { mode: actions?.partner ? 'partner' : actions ? 'scenario' : 'none',
      state: actions ? 'ready' : 'unavailable' };
    if (listening && batch && execution.mode === 'partner' && execution.state === 'ready' &&
        !actions?.partner?.claim(request.controller)) execution = { mode: 'partner', state: 'busy' };
    return { version: 1, id: request.id, inbox: batch && { ...batch, execution, ...(control && { control }),
      ...(sceneRevision != null && { sceneRevision }) } };
  }
  // 伙伴的全局接管必须表明范围，旧宿主的无范围停止不能释放角色。
  const takeover = Object.keys(request).length === 6 && request.scope === 'role';
  const scenarioStop = !actions?.partner && Object.keys(request).length === 5;
  if (request.method === 'control.stop' && actions && (takeover || scenarioStop) && typeof request.generation === 'string')
    return { version: 1, id: request.id, control: actions.releaseControl(request.generation) };
  if (request.method === 'control.detach' && actions?.partner && Object.keys(request).length === 6 && controller &&
      typeof request.generation === 'string')
    return { version: 1, id: request.id, control: { generation: request.generation, controller: request.controller,
      order: 0, phase: actions.partner.detach(request.controller) } };
  if (actions?.partner) {
    const guarded = request.skill === 'guarded_collect' && Array.isArray(request.arguments?.collectionGuards) &&
      request.arguments.collectionGuards.length > 0 && request.arguments.collectionGuards.length <= 128;
    const skill = guarded ? 'collect' : request.skill;
    if (!['partner.begin', 'partner.read', 'partner.stop'].includes(request.method) ||
      !['observe', 'move', 'route', 'follow', 'collect', 'place', 'fill_target', 'reply'].includes(skill)) throw new Error('不是伙伴会话的能力');
    const receipt = actions.partner.call(request.method, request.id, request.context, skill, request.arguments);
    return { version: 1, id: request.id, action: receipt };
  }
  if (actions?.deliveries && request.method === 'collection.admit' && Object.keys(request).length === 5 && request.grant &&
    typeof request.grant === 'object' && Object.keys(request.grant).length === 10)
    return { version: 1, id: request.id, delivery: actions.deliveries.admit(request.grant) };
  if (actions?.deliveries && request.method === 'collection.receipt' && Object.keys(request).length === 7 &&
    typeof request.generation === 'string' && typeof request.question === 'string' && typeof request.grant === 'string')
    return { version: 1, id: request.id, delivery: actions.deliveries.read(request.generation, request.question, request.grant) };
  if (actions?.mining && Object.keys(request).length === 8 && Number.isSafeInteger(request.index) && request.index > 0) {
    if (request.method === 'mine.begin') return { version: 1, id: request.id,
      action: actions.mining.begin(request.generation, request.question, request.grant, request.index) };
    if (['mine.read', 'mine.stop'].includes(request.method)) return { version: 1, id: request.id,
      action: actions.mining.poll(request.generation, request.question, request.grant, request.index, request.method === 'mine.stop') };
  }
  if (actions?.mining && request.method === 'mine.revoke' && Object.keys(request).length === 7)
    return { version: 1, id: request.id, withdrawal: actions.mining.revoke(request.generation, request.question, request.grant) };
  if (actions?.returning && Object.keys(request).length === 7 && typeof request.grant === 'string' && typeof request.question === 'string') {
    if (request.method === 'return.begin') return { version: 1, id: request.id,
      action: actions.returning.begin(request.generation, request.question, request.grant) };
    if (['return.read', 'return.stop'].includes(request.method)) return { version: 1, id: request.id,
      action: actions.returning.poll(request.generation, request.question, request.grant, request.method === 'return.stop') };
  }
  if (actions && typeof request.generation === 'string' && Number.isSafeInteger(request.order) && request.order > 0) {
    if (request.method === 'question.open' && Object.keys(request).length === 7 && typeof request.question === 'string')
      return { version: 1, id: request.id, question: actions.ask(request.generation, request.order, request.question) };
    if (request.method === 'inspect.begin' && Object.keys(request).length === 7 && typeof request.place === 'string')
      return { version: 1, id: request.id, action: actions.begin(request.generation, request.order, request.place) };
    if (['inspect.read', 'inspect.stop'].includes(request.method) && Object.keys(request).length === 6)
      return { version: 1, id: request.id, action: actions.poll(request.generation, request.order, request.method === 'inspect.stop') };
  }
  throw new Error('invalid MC method');
}

// 在本机管道上接受一次一份的只读请求，关闭时使全部连接及原世界失效。
// 步骤：限制请求长度与等待 → 校验协议并返回快照 → 关闭入口并等待连接释放。
async function openBridge(channel, world, inbox = null, key = null, actions = null) {
  const sockets = new Set();
  const server = net.createServer(socket => {
    sockets.add(socket);
    socket.on('error', () => {});
    socket.once('close', () => sockets.delete(socket));
    socket.setTimeout(3000, () => socket.destroy());
    let message = Buffer.alloc(0);
    let replied = false;
    // 按字节累积一行并拒绝同连接上的额外请求。
    socket.on('data', chunk => {
      if (replied || message.length + chunk.length > 128 * 1024) return socket.destroy();
      message = Buffer.concat([message, chunk]);
      const newline = message.indexOf(10);
      if (newline === -1) return;
      if (newline !== message.length - 1) return socket.destroy();
      replied = true;
      try {
        const reply = readReply(new TextDecoder('utf-8', { fatal: true }).decode(message.subarray(0, newline)), world, inbox, key, actions);
        socket.end(JSON.stringify(reply) + '\n');
      } catch {
        socket.destroy();
      }
    });
  });
  server.maxConnections = 8;
  try {
    server.listen(pipeName(channel));
    await once(server, 'listening');
  } catch (error) {
    await actions?.close();
    inbox?.close();
    world.close();
    throw error;
  }
  return {
    // 立即撤销观察绑定并等待管道停止监听，允许同一调用者重复关闭。
    async close() {
      const stopping = actions?.close();
      inbox?.close();
      world.close();
      if (!server.listening) { await stopping; return; }
      const closed = once(server, 'close');
      server.close();
      for (const socket of sockets) socket.destroy();
      await Promise.all([closed, stopping]);
    },
  };
}

module.exports = { openBridge, pipeName };
