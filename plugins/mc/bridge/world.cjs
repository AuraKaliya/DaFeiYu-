const { randomUUID } = require('node:crypto');

// 返回实体脚位所在格正下方的整数格，负坐标也向下取整。
function belowFeet(point) { return point.floored().offset(0, -1, 0); }

// 为当前出生到撤销之间的游戏世界建立只读绑定，记录最近的服务端信号。
// 步骤：绑定游戏事件 → 按代次复制角色状态 → 撤销后丢弃旧世界。
function observeWorld(bot) {
  let generation = null;
  let level = null;
  const maps = new Map(), spawnMaps = new Map();
  let lastSignal = 0;
  let closed = false;
  const signal = () => { lastSignal = performance.now(); };
  const enter = () => { generation = randomUUID(); spawnMaps.clear(); signal(); };
  const leave = () => { generation = null; spawnMaps.clear(); };
  // 从服务端出生信息读取实际地图名，区分同维度类型的不同地图。
  const readLevel = packet => { level = packet.worldState?.name || null; };
  // 新登录隔离上一条连接，即使两个服务器选用了相同的地图名。
  const login = packet => { maps.clear(); readLevel(packet); };
  const disconnect = () => { leave(); maps.clear(); level = null; };
  bot._client?.on('login', login);
  bot._client?.on('respawn', readLevel);
  bot.on('spawn', enter);
  bot.on('respawn', leave);
  bot.on('end', disconnect);
  bot.on('time', signal);
  bot.on('forcedMove', signal);
  if (bot.entity && bot.player?.uuid) enter();

  return {
    // 为同一连接到过的地图提供可复用的对象身份。
    // 步骤：核对出生和维度 → 或按地图名、或按本次出生保留独立身份。
    space() {
      if (closed || !generation || !bot.game?.dimension) return null;
      const index = level ? maps : spawnMaps;
      const name = JSON.stringify([level, bot.game.dimension]);
      if (!index.has(name)) index.set(name, Object.freeze({ level, dimension: bot.game.dimension }));
      return index.get(name);
    },
    // 只复制客户端能取得的身份、位置和库存，轮询本身不刷新服务端信号时间。
    read() {
      if (closed || !generation || !bot.entity || !bot.player?.uuid) return null;
      const { x, y, z } = bot.entity.position;
      if (![x, y, z].every(Number.isFinite)) return null;
      const cobblestone = bot.inventory.items().filter(item => item.name === 'cobblestone')
        .reduce((sum, item) => sum + item.count, 0);
      return { generation, actor: bot.player.uuid, dimension: bot.game.dimension, position: { x, y, z },
        cobblestone, signalAgeMs: Math.max(0, Math.floor(performance.now() - lastSignal)) };
    },
    // 停止服务端事件投影，已有快照仅保留其原世界身份。
    close() {
      closed = true;
      disconnect();
      bot._client?.removeListener('login', login);
      bot._client?.removeListener('respawn', readLevel);
      bot.removeListener('spawn', enter);
      bot.removeListener('respawn', leave);
      bot.removeListener('end', disconnect);
      bot.removeListener('time', signal);
      bot.removeListener('forcedMove', signal);
    },
  };
}

module.exports = { observeWorld, belowFeet };
