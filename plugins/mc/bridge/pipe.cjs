const net = require('node:net');
const { once } = require('node:events');

// 从本机通道短名构造固定命名空间，禁止传入路径。
function pipeName(channel) {
  if (typeof channel !== 'string' || !/^[a-zA-Z0-9-]{1,48}$/.test(channel)) throw new Error('invalid MC channel');
  return '\\\\.\\pipe\\DaFeiYu.MC.' + channel;
}

// 处理一份有界状态查询，编号只用于关联，不承担授权含义。
function readReply(input, world) {
  const request = JSON.parse(input);
  if (!request || request.version !== 1 || request.method !== 'state.read' ||
    typeof request.id !== 'string' || !/^[0-9a-zA-Z.-]{1,96}$/.test(request.id) ||
    Object.keys(request).length !== 3) throw new Error('invalid MC request');
  return { version: 1, id: request.id, state: world.read() };
}

// 在本机管道上接受一次一份的只读请求，关闭时使全部连接及原世界失效。
// 步骤：限制请求长度与等待 → 校验协议并返回快照 → 关闭入口并等待连接释放。
async function openBridge(channel, world) {
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
      if (replied || message.length + chunk.length > 2048) return socket.destroy();
      message = Buffer.concat([message, chunk]);
      const newline = message.indexOf(10);
      if (newline === -1) return;
      if (newline !== message.length - 1) return socket.destroy();
      replied = true;
      try {
        const reply = readReply(new TextDecoder('utf-8', { fatal: true }).decode(message.subarray(0, newline)), world);
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
    world.close();
    throw error;
  }
  return {
    // 立即撤销观察绑定并等待管道停止监听，允许同一调用者重复关闭。
    async close() {
      world.close();
      if (!server.listening) return;
      const closed = once(server, 'close');
      server.close();
      for (const socket of sockets) socket.destroy();
      await closed;
    },
  };
}

module.exports = { openBridge, pipeName };
