const assert = require('node:assert/strict');
const { writeFileSync } = require('node:fs');
const { join } = require('node:path');
const { randomUUID } = require('node:crypto');
const { withPaper } = require('./paper.cjs');
const { withPlayers, until } = require('./players.cjs');
const { readStream } = require('./origins.cjs');
const { verifyCase, stoneCount } = require('./pickups.cjs');
const { ownPickupRange } = require('./pair-evidence.cjs');

// 从独立服务器线程和真实客户端分别取得同一角色的最新读数。
async function sample(fixture, fish) {
  const request = randomUUID();
  fixture.send('dfystonesample ' + fish.username + ' ' + request);
  const value = await until(() => readStream(fixture.server).find(row => row.kind === 'sample' &&
    row.request === request && row.actor === fish.player.uuid), '服务器采样回执');
  assert.equal(value.stock, stoneCount(fish));
  assert.ok(fish.entity.position.distanceTo({ x: value.x, y: value.y, z: value.z }) < 1);
  return { server: value, client: { position: fish.entity.position.clone(), stock: stoneCount(fish) } };
}

// 仅从连续持有证据推导这次混合堆丢物后的自采范围。
// 步骤：验证原拾取和同世界的到达采样 → 确认唯一实际丢物 → 缩减数量上下界。
function carried(rows, self, gift, actor, before, arrival) {
  if (before.seq >= arrival.seq || before.world !== arrival.world || before.world !== self.world ||
    before.actor !== actor || arrival.actor !== actor || Math.abs(before.x) <= 8 ||
    Math.abs(arrival.x) > 8 || Math.abs(arrival.z) > 8 || arrival.y < -61 || arrival.y > -58) return null;
  if (rows.at(-1)?.seq !== arrival.seq || rows.some((row, index) =>
    row.stream !== self.stream || row.seq !== rows[0].seq + index)) return null;
  const acquired = ownPickupRange(rows.filter(row => row.seq <= before.seq), self, gift, actor);
  if (!acquired || before.stacks.length !== 1 || before.stacks[0] !== before.stock) return null;
  const later = rows.filter(row => row.seq > before.seq);
  const losses = later.filter(row => row.kind === 'playerDrop' && row.actor === actor);
  if (losses.length !== 1 || later.some(row => row.kind === 'pickup' && row.actor === actor)) return null;
  const loss = losses[0];
  if (!later.some(row => row.kind === 'added' && row.item === loss.item && row.amount === loss.amount) ||
    before.stock - arrival.stock !== loss.amount) return null;
  return [Math.max(0, acquired[0] - loss.amount), Math.min(acquired[1], arrival.stock)];
}

// 由真实角色从取料点步行回到河边会合区，并核验混合库存的证据边界。
// 步骤：确认 5→7 的拾取来源 → 从同一堆实际丢一块 → 朝会合点行走 → 以新采样验证 6 与来源范围。
async function verifyReturn(fixture, owner, fish) {
  const pair = await verifyCase(fixture, owner, fish, 'full', 5);
  const before = await sample(fixture, fish);
  assert.equal(before.server.stock, 7);
  // 朝返程相反方向丢出，避免把刚丢下的物品重新捡起。
  await fish.lookAt(fish.entity.position.offset(8, fish.entity.height, 0), true);
  await fish.waitForTicks(2);
  await fish.toss(fish.registry.itemsByName.cobblestone.id, null, 1);
  await fish.lookAt(fish.entity.position.offset(-8, fish.entity.height, 0), true);
  fish.setControlState('forward', true);
  try {
    await until(() => fish.entity.position.x < 7.5, '角色实际走入河边');
  } finally {
    fish.clearControlStates();
  }
  const arrival = await sample(fixture, fish);
  assert.equal(arrival.client.stock, 6);
  const rows = readStream(fixture.server).slice(pair.cursor).filter(row => row.seq <= arrival.server.seq);
  const check = evidence => carried(evidence, pair.self, pair.gift, fish.player.uuid, before.server, arrival.server);
  assert.deepEqual(check(rows), [0, 1]);
  const lost = rows.find(row => row.seq > before.server.seq && row.kind === 'playerDrop');
  assert.equal(check(rows.filter(row => row.seq !== lost.seq)), null);
  writeFileSync(join(fixture.evidence, 'return.json'), JSON.stringify({ before, arrival, acquired: pair.range,
    carried: [0, 1], rows }, null, 2));
}

withPaper(fixture => withPlayers(fixture.port, (owner, fish) => verifyReturn(fixture, owner, fish)))
  .catch(error => { console.error(error); process.exitCode = 1; });
