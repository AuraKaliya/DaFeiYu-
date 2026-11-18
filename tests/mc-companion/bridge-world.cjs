const assert = require('node:assert/strict');
const net = require('node:net');
const { randomUUID } = require('node:crypto');
const { join } = require('node:path');
const { writeFileSync } = require('node:fs');
const { withPaper } = require('./paper.cjs');
const { withPlayers } = require('./players.cjs');
const { observeWorld } = require('../../plugins/mc/bridge/world.cjs');
const { openBridge, pipeName } = require('../../plugins/mc/bridge/pipe.cjs');

// 从独立连接发出固定请求并等到完整响应或原生断开。
function query(channel, request) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(pipeName(channel));
    let data = '';
    socket.setTimeout(3000, () => socket.destroy(new Error('query timeout')));
    socket.on('error', reject);
    socket.on('connect', () => socket.write(JSON.stringify(request) + '\n'));
    socket.on('data', chunk => { data += chunk.toString(); });
    socket.on('end', () => resolve(data ? JSON.parse(data) : null));
  });
}

// 从真实 Mineflayer 状态读取有界快照，验证撤销和写方法拒绝。
// 步骤：创建独立读端 → 验证身份、位置及无效请求 → 撤销旧来源并关闭。
async function check(fixture, fish) {
  const world = observeWorld(fish);
  const channel = randomUUID();
  const bridge = await openBridge(channel, world);
  const request = { version: 1, method: 'state.read', id: '1' };
  try {
    const reply = await query(channel, request);
    assert.equal(reply.id, request.id);
    assert.equal(reply.state.actor, fish.player.uuid);
    assert.equal(reply.state.dimension, 'overworld');
    assert.ok(Math.abs(reply.state.position.x - fish.entity.position.x) < 1);
    assert.equal(await query(channel, { ...request, method: 'world.mine' }), null);
    world.close();
    assert.equal((await query(channel, request)).state, null);
    writeFileSync(join(fixture.evidence, 'bridge.json'), JSON.stringify(reply, null, 2));
  } finally {
    await bridge.close();
  }
  await assert.rejects(query(channel, request));
}

withPaper(fixture => withPlayers(fixture.port, (_owner, fish) => check(fixture, fish)))
  .catch(error => { console.error(error); process.exitCode = 1; });
