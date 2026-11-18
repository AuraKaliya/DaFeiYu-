const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const { join } = require('node:path');
const { writeFileSync } = require('node:fs');
const { launch, readConfig } = require('../../plugins/mc/bridge/start.cjs');
const { withPaper } = require('./paper.cjs');
const { until } = require('./players.cjs');
const { query } = require('./bridge-world.cjs');

// 使用普通启动入口创建独立账号连接并验证连接结束后的管道退出。
// 步骤：准备独立配置 → 读取同一出生代次的快照 → 停止并拒绝旧读口。
async function check(fixture) {
  const channel = randomUUID();
  const file = join(fixture.evidence, 'connection.json');
  const config = { channel, host: '127.0.0.1', port: fixture.port, username: 'DfyCompanion', auth: 'offline' };
  writeFileSync(file, JSON.stringify({ ...config, host: 'example.invalid' }));
  assert.throws(() => readConfig(file));
  writeFileSync(file, JSON.stringify(config));
  const session = await launch(file);
  try {
    await until(() => session.bot.player?.uuid && session.bot.entity && Number.isFinite(session.bot.entity.position.x),
      '独立适配器真实登录');
    const request = { version: 1, method: 'state.read', id: 'adapter' };
    let result;
    for (let round = 0; round < 40 && !result?.state; round++) {
      result = await query(channel, request);
      if (!result?.state) await session.bot.waitForTicks(1);
    }
    assert.equal(result?.state?.actor, session.bot.player.uuid);
    assert.equal(result.state.dimension, 'overworld');
    writeFileSync(join(fixture.evidence, 'readback.json'), JSON.stringify(result, null, 2));
  } finally {
    session.stop();
    await session.done;
  }
  await assert.rejects(query(channel, { version: 1, method: 'state.read', id: 'stopped' }));
}

withPaper(check).catch(error => { console.error(error); process.exitCode = 1; });
