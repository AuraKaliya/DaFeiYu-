const { randomUUID } = require('node:crypto');

// 为当前出生到撤销之间的游戏世界建立只读绑定，记录最近的服务端信号。
// 步骤：绑定游戏事件 → 按代次复制角色状态 → 撤销后丢弃旧世界。
function observeWorld(bot) {
  let generation = null;
  let lastSignal = 0;
  let closed = false;
  const signal = () => { lastSignal = performance.now(); };
  const enter = () => { generation = randomUUID(); signal(); };
  const leave = () => { generation = null; };
  bot.on('spawn', enter);
  bot.on('respawn', leave);
  bot.on('end', leave);
  bot.on('time', signal);
  bot.on('forcedMove', signal);
  if (bot.entity && bot.player?.uuid) enter();

  return {
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
      leave();
      bot.removeListener('spawn', enter);
      bot.removeListener('respawn', leave);
      bot.removeListener('end', leave);
      bot.removeListener('time', signal);
      bot.removeListener('forcedMove', signal);
    },
  };
}

module.exports = { observeWorld };
