const { spawn } = require('node:child_process');
const { randomUUID } = require('node:crypto');
const { once } = require('node:events');
const net = require('node:net');
const { pipeName } = require('../../plugins/mc/bridge/pipe.cjs');

// 用真实本机管道产生分片、断线、超限和不回包四种行为。
async function main() {
  const channel = randomUUID();
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
