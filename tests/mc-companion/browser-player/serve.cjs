const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const net = require('node:net');
const { Transform } = require('node:stream');
const { randomUUID } = require('node:crypto');
const express = require('express');
const { WebSocketServer, createWebSocketStream } = require('ws');

const root = process.env.DFY_MC_TEST_ROOT;
if (!root || !path.isAbsolute(root)) throw new Error('请指定本机体验根目录');
const dist = path.join(root, 'mwc/2.4.0/dist');
const origin = 'http://127.0.0.1:25582';
const address = '127.0.0.1:25582';
const client = fs.readFileSync(path.join(dist, 'index.html'), 'utf8');
const app = express();
const server = http.createServer(app);
const sockets = new WebSocketServer({ noServer: true, maxPayload: 2 * 1024 * 1024 });
const options = new URLSearchParams({ ip: '127.0.0.1:25580', proxy: origin, version: '1.21.4',
  username: 'DfyVisitor', onlyConnect: '1', connectText: '进入河边体验' });

// 只向本机发出网页和游戏数据，进入按钮由上游客户端处理。
app.use((req, res, next) => {
  if (req.headers.host !== address) return res.sendStatus(403);
  res.setHeader('Referrer-Policy', 'no-referrer');
  next();
});
app.get('/', (req, res) => {
  if (!req.query.ip) return res.redirect('/?' + options);
  res.type('html').send(client.replace(/<script\b[^>]*src="https:\/\/cloud\.umami\.is[^"]*"[^>]*><\/script>/g, '')
    .replace('</body>', '<script src="/appearance.js"></script></body>'));
});
app.get('/appearance.js', (req, res) => res.sendFile(path.join(__dirname, 'appearance.js')));
app.get('/health', (req, res) => res.json({ service: 'dafeiyu-webmc', version: '2.4.0' }));
app.get('/companion.png', (req, res) => res.sendFile(path.join(root, 'appearance/DeepSeek-926524.png')));
app.get('/config.json', (req, res) => res.json({ defaultProxies: [], promoteServers: [], defaultLanguage: 'zh-CN',
  defaultSettings: { renderDistance: 4, serverResourcePacks: 'never', loadPlayerSkins: false } }));
app.use(express.static(dist, { index: false }));

const tickets = new Set();
// 按上游 net-browserify 的两段连接协议配对，只允许本页申请连接固定体验服。
app.post('/api/vm/net/connect', express.json({ limit: '2kb' }), (req, res) => {
  if (req.headers.origin !== origin || req.body?.host !== '127.0.0.1' || Number(req.body.port) !== 25580 ||
      tickets.size >= 8) return res.sendStatus(403);
  const token = randomUUID();
  tickets.add(token);
  setTimeout(() => tickets.delete(token), 10000).unref();
  res.json({ token, remote: { address: '127.0.0.1', family: 'IPv4', port: 25580 } });
});
// 消费单次握手票据，将玩家自己的协议流连接到唯一的本机游戏目标。
server.on('upgrade', (req, socket, head) => {
  const url = URL.parse(req.url, origin);
  if (req.headers.host !== address || req.headers.origin !== origin || url?.pathname !== '/api/vm/net/socket' ||
      !tickets.delete(url.searchParams.get('token'))) {
    socket.destroy();
    return;
  }
  sockets.handleUpgrade(req, socket, head, ws => {
    const game = net.createConnection({ host: '127.0.0.1', port: 25580 });
    const stream = createWebSocketStream(ws, { readableObjectMode: true });
    // 文本心跳由浏览器的网络层消费，游戏只接收二进制协议数据。
    const packets = new Transform({ writableObjectMode: true, transform(data, encoding, done) {
      if (typeof data !== 'string') return done(null, data);
      if (/^ping:[\w.-]{1,80}$/.test(data)) ws.send('pong:' + data.slice(5));
      done();
    } });
    game.on('error', () => stream.destroy());
    stream.on('error', () => game.destroy());
    stream.on('close', () => game.destroy());
    game.on('close', () => stream.destroy());
    stream.pipe(packets).pipe(game).pipe(stream);
  });
});
server.listen(25582, '127.0.0.1', () => console.log('浏览器玩家入口 ' + origin));
