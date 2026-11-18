const { createRequire } = require('node:module');
const { resolve } = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');
const bridgeRequire = createRequire(resolve(__dirname, '../../plugins/mc/bridge/package.json'));
const mineflayer = bridgeRequire('mineflayer');
const version = '1.21.4';

// 在限时内等待读数满足条件，断线或超时由调用者收尾。
// 步骤：读取当前条件 → 让出事件循环 → 直到获得结果或报告失败。
async function until(read, description, timeoutMs = 10000) {
  const deadline = performance.now() + timeoutMs;
  do {
    const value = read();
    if (value) return value;
    await delay(50);
  } while (performance.now() < deadline);
  throw new Error('等待超时：' + description);
}

// 在指定回环端口建立一个独立的本地测试身份。
// 步骤：启动协议连接 → 等待实际出生或错误 → 失败时释放连接。
async function connect(username, port) {
  const bot = mineflayer.createBot({ host: '127.0.0.1', port, username, auth: 'offline', version });
  let spawned = false;
  let failure;
  bot.once('spawn', () => { spawned = true; });
  bot.once('end', reason => { failure = new Error(username + ' disconnected: ' + reason); });
  bot.on('error', error => { failure = error; });
  try {
    await until(() => {
      if (failure) throw failure;
      return spawned && bot.player?.uuid;
    }, username + ' spawn', 30000);
    return bot;
  } catch (error) {
    bot.quit();
    throw error;
  }
}

// 在两个独立真实连接都可见后交给本次探针，退出时归还连接。
// 步骤：校验端口 → 依次连接、本服交叉确认 UUID → 执行探针并收尾。
async function withPlayers(port, probe) {
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('invalid fixture port');
  const owner = await connect('DfyOwner', port);
  try {
    const fish = await connect('DfyCompanion', port);
    try {
      await until(() => owner.players[fish.username]?.uuid === fish.player.uuid &&
        fish.players[owner.username]?.uuid === owner.player.uuid, '双角色同时在场');
      return await probe(owner, fish);
    } finally {
      fish.quit();
    }
  } finally {
    owner.quit();
  }
}

module.exports = { connect, until, withPlayers, version };
