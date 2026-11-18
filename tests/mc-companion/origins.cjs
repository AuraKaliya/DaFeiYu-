const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { withPaper } = require('./paper.cjs');
const { withPlayers, until } = require('./players.cjs');

// 只读当前观察实例已经写完的整行，尾部尚未完成的记录留待下一次读取。
function readStream(server) {
  const dir = path.join(server, 'plugins', 'DfyStoneObserver');
  const stream = fs.readFileSync(path.join(dir, 'active-stream'), 'utf8');
  assert.match(stream, /^[0-9a-f-]{36}$/);
  const raw = fs.readFileSync(path.join(dir, stream + '.jsonl'), 'utf8');
  const rows = raw.split(/\r?\n/).slice(0, -1).map(line => JSON.parse(line));
  rows.forEach((row, index) => { assert.equal(row.stream, stream); assert.equal(row.seq, index + 1); });
  return rows;
}

// 用本地控制台布置夹具并等待两个协议角色实际收到位置、材料和工具。
// 步骤：准备场地、角色与物资 → 等待客户端世界更新 → 取出并装备工具。
async function prepare(fixture, owner, fish) {
  for (const command of [
    'kill @e[type=item]', 'clear DfyOwner', 'clear DfyCompanion',
    'tp DfyOwner -1.5 -60 0.5', 'tp DfyCompanion -1.5 -60 0.5',
    'setblock 1 -60 0 minecraft:cobblestone', 'give DfyOwner minecraft:cobblestone 1',
    'give DfyCompanion minecraft:iron_pickaxe 1',
  ]) fixture.send(command);
  const target = fish.entity.position.clone().set(1, -60, 0);
  await until(() => fish.entity.position.x === -1.5 && owner.entity.position.x === -1.5 &&
    fish.blockAt(target)?.name === 'cobblestone' && owner.inventory.items().some(item => item.name === 'cobblestone') &&
    fish.inventory.items().some(item => item.name === 'iron_pickaxe'), '收到本轮夹具');
  await fish.equip(fish.inventory.items().find(item => item.name === 'iron_pickaxe'), 'hand');
  await owner.lookAt(target.offset(0.5, 0.3, 0.5), true);
  return target;
}

// 在同一次采集窗口中执行真实挖掘和玩家丢物，并核对两份地面实体来源。
// 步骤：取双端基线 → 并发执行动作 → 用实例及实体编号对齐 → 保存原始观测。
async function verifyOrigins(fixture, owner, fish) {
  const target = await prepare(fixture, owner, fish);
  const cursor = readStream(fixture.server).length;
  const clientDrops = new Map();
  fish.on('itemDrop', entity => {
    const item = entity.getDroppedItem();
    if (item?.name === 'cobblestone') clientDrops.set(entity.id, { id: entity.id, count: item.count });
  });
  await Promise.all([fish.dig(fish.blockAt(target)), owner.tossStack(owner.inventory.items().find(item => item.name === 'cobblestone'))]);
  const records = await until(() => {
    const rows = readStream(fixture.server).slice(cursor);
    const self = rows.find(row => row.kind === 'blockDrop' && row.actor === fish.player.uuid);
    const gift = rows.find(row => row.kind === 'playerDrop' && row.actor === owner.player.uuid);
    if (!self || !gift) return false;
    const entered = [self, gift].every(source => clientDrops.has(source.networkId) &&
      rows.some(row => row.kind === 'added' && row.world === source.world && row.item === source.item &&
        row.networkId === source.networkId && row.amount === 1));
    return entered && { self, gift, rows };
  }, '两种来源各自实际进入游戏并可在客户端看见');
  const { self, gift, rows } = records;
  assert.deepEqual([self.x, self.y, self.z], [target.x, target.y, target.z]);
  assert.equal(self.amount, 1);
  assert.equal(gift.amount, 1);
  assert.equal(self.world, gift.world);
  assert.notEqual(self.item, gift.item);
  assert.notEqual(self.networkId, gift.networkId);
  assert.ok(rows.some(row => row.kind === 'added' && (
    (row.item === self.item && row.nearbyStone.includes(gift.item)) ||
    (row.item === gift.item && row.nearbyStone.includes(self.item)))), '两份掉落需要在首次拾取前同场');
  assert.ok(rows.some(row => row.kind === 'blockBroken' && row.actor === fish.player.uuid && row.candidateAmount === 1));
  fs.writeFileSync(path.join(fixture.evidence, 'origins.json'), JSON.stringify({
    actors: { fish: fish.player.uuid, owner: owner.player.uuid }, target, clientDrops: [...clientDrops.values()], rows,
  }, null, 2));
  console.log('已对齐独立来源与地面实体：', self.networkId, gift.networkId);

  fixture.send('setblock 1 -60 0 minecraft:cobblestone');
  await until(() => fish.blockAt(target)?.name === 'cobblestone', '负例目标就绪');
  await fish.unequip('hand');
  const before = readStream(fixture.server).length;
  await fish.dig(fish.blockAt(target));
  const empty = await until(() => {
    const later = readStream(fixture.server).slice(before);
    return later.some(row => row.kind === 'blockBroken' && row.actor === fish.player.uuid &&
      row.candidateAmount === 0) && later;
  }, '徒手破坏实际产生零圆石');
  assert.ok(!empty.some(row => row.kind === 'blockDrop'));
  fs.writeFileSync(path.join(fixture.evidence, 'no-drop.json'), JSON.stringify(empty, null, 2));
}

if (require.main === module) {
  withPaper(fixture => withPlayers(fixture.port, (owner, fish) => verifyOrigins(fixture, owner, fish)))
    .catch(error => { console.error(error); process.exitCode = 1; });
}
module.exports = { readStream, prepare };
