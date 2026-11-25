const { spawn } = require('node:child_process');
const { randomUUID } = require('node:crypto');
const { once } = require('node:events');
const net = require('node:net');
const { pipeName } = require('../bridge/pipe.cjs');

// 用真实本机管道产生分片、断线、超限和不回包四种行为。
async function main() {
  const channel = randomUUID();
  const generation = randomUUID(), actor = randomUUID(), owner = randomUUID();
  let inboxReads = 0;
  const server = net.createServer(socket => {
    socket.on('error', () => {});
    socket.once('data', data => {
      switch (data.toString().trim()) {
        case 'parts':
          socket.write('x'.repeat(3000));
          setImmediate(() => socket.end('x'.repeat(2000) + '\n'));
          break;
        case 'oversized': socket.end('x'.repeat(17000) + '\n'); break;
        case 'disconnect': socket.end('partial'); break;
        case 'silent': break;
        default: {
          const request = JSON.parse(data.toString());
          if (request.method === 'state.read') {
            socket.end(JSON.stringify({ version: 1, id: request.id, state: { generation, actor, dimension: 'overworld',
              position: { x: -1, y: -60, z: 0 }, cobblestone: 3, signalAgeMs: 10 } }) + '\n');
          } else if (request.method === 'partner.listen') {
            if (request.generation !== generation || request.key !== '1'.repeat(64) ||
                !/^[a-f0-9]{32}$/.test(request.controller)) return socket.destroy();
            const presence = { sequence: 2, type: 'player.entered' };
            const initial = ++inboxReads === 1;
            const messages = initial ? [{ sequence: 1, text: 'old speech' }, presence] :
              request.after === 2 ? [{ sequence: 3, text: 'current speech' }] : [];
            socket.end(JSON.stringify({ version: 1, id: request.id, inbox: { generation, owner, oldest: 1,
              latest: initial ? 2 : 3, gap: false, more: false, presence, messages } }) + '\n');
          } else socket.destroy();
        }
      }
    });
  });
  server.listen(pipeName(channel));
  await once(server, 'listening');
  try {
    const child = spawn(process.argv[2], [channel], { stdio: 'inherit', windowsHide: true });
    const [code] = await once(child, 'exit');
    if (code !== 0) throw new Error('native pipe probe failed');
  } finally {
    server.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
