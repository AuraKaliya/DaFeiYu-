const fs = require('node:fs');
const path = require('node:path');
const mineflayer = require('mineflayer');
const { openBridge, pipeName } = require('./pipe.cjs');
const { observeWorld } = require('./world.cjs');
const { ownerInbox } = require('./chat.cjs');
const { inspectionService, landmarks, inspectionOffer } = require('./inspection.cjs');
const { stoneWitness } = require('./stone-evidence.cjs');

// 从用户选择的配置读取单个游戏角色，离线身份限回环夹具。
// 步骤：校验配置与通道 → 选择独立角色的认证方式 → 将缓存定位到配置所在目录。
function readConfig(file) {
  const input = JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
  const { channel, host, port, username, auth } = input;
  pipeName(channel);
  if (typeof host !== 'string' || !host || host.length > 253 || !Number.isInteger(port) || port < 1 || port > 65535 ||
    typeof username !== 'string' || !username.trim() || username.length > 254) throw new Error('无效游戏连接配置');
  if (auth !== 'microsoft' && !(auth === 'offline' && host === '127.0.0.1'))
    throw new Error('独立账号使用 microsoft，本机夹具可使用 offline');
  if (input.owner !== undefined && (typeof input.owner !== 'string' ||
    !/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/.test(input.owner))) throw new Error('无效玩家 UUID');
  if (input.owner && (typeof input.bridgeKey !== 'string' || !/^[a-f0-9]{64}$/.test(input.bridgeKey)))
    throw new Error('读取玩家私有消息需要独立桥接配置');
  return { channel, owner: input.owner ?? null, key: input.bridgeKey ?? null, places: landmarks(input.places),
    witness: stoneWitness(input.observer), game: { host, port, username, auth, version: '1.21.4',
    profilesFolder: path.join(path.dirname(path.resolve(file)), 'profiles') } };
}

// 将同一个实际游戏连接装配成可撤销的本机桥接，退出时归还管道与客户端。
// 步骤：建立角色和可见状态 → 发布管道 → 在主动停止或断开时收尾。
async function launch(file) {
  const { channel, game, owner, key, places, witness } = readConfig(file);
  const bot = mineflayer.createBot(game);
  let gameError;
  bot.on('error', error => { gameError = error; });
  const ended = new Promise(resolve => bot.once('end', resolve));
  const world = observeWorld(bot);
  let actions;
  const inbox = ownerInbox(bot, world, owner, game.auth === 'offline' && game.host === '127.0.0.1', 32,
    (text, generation, sequence) => actions?.playerCommand(text, generation, sequence) ?? inspectionOffer(text, places));
  actions = places.size && owner ? inspectionService(bot, world, inbox, places, witness) : null;
  let bridge;
  try {
    bridge = await openBridge(channel, world, inbox, key, actions);
  } catch (error) {
    bot.end();
    throw error;
  }
  const done = ended.then(async () => {
    await bridge.close();
    if (gameError) throw gameError;
  });
  return { bot, done, stop: () => bot.end() };
}

// 启动配置指定的陪伴连接，控制台信号只结束本进程的角色。
async function main() {
  if (process.argv.length !== 3) throw new Error('用法：node start.cjs <游戏连接配置.json>');
  const session = await launch(process.argv[2]);
  process.once('SIGINT', session.stop);
  process.once('SIGTERM', session.stop);
  try { await session.done; }
  finally {
    process.removeListener('SIGINT', session.stop);
    process.removeListener('SIGTERM', session.stop);
  }
}

if (require.main === module) main().catch(error => { console.error(error.message); process.exitCode = 1; });
module.exports = { launch, readConfig };
