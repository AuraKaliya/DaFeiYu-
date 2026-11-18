const net = require('node:net');
const { once } = require('node:events');
const { pipeName } = require('../../plugins/mc/bridge/pipe.cjs');
const { query } = require('./bridge-world.cjs');

// 仅在选定的一次接单帧上注入真实断连，控制端仍须通过正式只读回查恢复。
// 步骤：截取本地协议帧 → 在转发前或实际回复后断开 → 完整转发其余调用并记录方法。
async function deliveryProxy(channel, target, variant) {
  const methods = [];
  const sockets = new Set();
  let dropped = false;
  const server = net.createServer(socket => {
    sockets.add(socket);
    socket.once('close', () => sockets.delete(socket));
    socket.on('error', () => {});
    let bytes = '';
    socket.on('data', async chunk => {
      try {
        bytes += chunk.toString();
        if (bytes.length > 2048) return socket.destroy();
        if (!bytes.endsWith('\n')) return;
        const request = JSON.parse(bytes);
        if (request.method.startsWith('collection.')) methods.push(request.method);
        const fault = !dropped && request.method === 'collection.admit';
        if (fault) dropped = true;
        if (fault && variant === 'lost-send') return socket.destroy();
        const result = await query(target, request);
        if (fault && variant === 'lost-receipt') return socket.destroy();
        if (!result) return socket.destroy();
        socket.end(JSON.stringify(result) + '\n');
      } catch { socket.destroy(); }
    });
  });
  server.listen(pipeName(channel));
  await once(server, 'listening');
  return {
    methods,
    // 关闭故障注入入口并回收所有临时连接。
    async close() {
      const done = once(server, 'close');
      server.close();
      for (const socket of sockets) socket.destroy();
      await done;
    },
  };
}
module.exports = { deliveryProxy };
