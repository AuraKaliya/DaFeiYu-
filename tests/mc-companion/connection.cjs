const assert = require('node:assert/strict');
const { writeFileSync } = require('node:fs');
const { join } = require('node:path');
const { until, withPlayers, version } = require('./players.cjs');

// 复制角色的协议身份、维度、坐标、会合区和当前库存。
function readPlayer(bot) {
  const { x, y, z } = bot.entity.position;
  return {
    name: bot.username, uuid: bot.player.uuid, entity: bot.entity.id,
    dimension: bot.game.dimension, position: { x, y, z },
    atMeeting: Math.abs(x) <= 8 && Math.abs(z) <= 8 && y >= -61 && y <= -58,
    inventory: bot.inventory.items().map(item => ({ slot: item.slot, name: item.name, count: item.count })),
  };
}

// 在回环世界核对两个玩家与会合区的基线并写出独立读数。
// 步骤：等场地就绪 → 检查角色身份和地面 → 保存同场证据。
async function capture(owner, fish, destination) {
  const ground = await until(() => {
    const floor = fish.blockAt(fish.entity.position.floored().offset(0, -1, 0));
    return floor && floor.boundingBox === 'block' && floor;
  }, '读取脚下的实际地面');
  const players = [readPlayer(owner), readPlayer(fish)];
  assert.notEqual(players[0].uuid, players[1].uuid);
  assert.notEqual(players[0].entity, players[1].entity);
  assert.ok(players.every(player => player.dimension === 'overworld' && player.atMeeting));
  const evidence = { version: 1, gameVersion: version, ground: ground.name, players };
  const json = JSON.stringify(evidence, null, 2);
  if (destination) writeFileSync(destination, json + '\n', { flag: 'wx' });
  console.log(json);
}

// 按调用方式复用已启动的回环服，或管理本次独占测试服的完整寿命。
async function main() {
  if (process.argv[2] === '--managed') {
    const { withPaper } = require('./paper.cjs');
    return withPaper(({ port, evidence }) => withPlayers(port,
      (owner, fish) => capture(owner, fish, join(evidence, 'clients.json'))));
  }
  return withPlayers(Number(process.argv[2]), (owner, fish) => capture(owner, fish, process.argv[3]));
}

main().catch(error => { console.error(error); process.exitCode = 1; });
