const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const { once } = require('node:events');
const { createConnection } = require('node:net');
const { test } = require('node:test');
const { controllerLease } = require('../../plugins/mc/bridge/controller.cjs');
const { openBridge, pipeName } = require('../../plugins/mc/bridge/pipe.cjs');

// 用同一个私有读口竞争角色，原工作完成前保留旧实例及回执。
// 步骤：读取和登记 → 退出时等待原工作 → 新实例接管并重试旧释放。
test('a scoped detach waits for work and cannot release the next controller', async t => {
  const channel = randomUUID(), generation = randomUUID(), key = '1'.repeat(64);
  const a = 'a'.repeat(32), b = 'b'.repeat(32);
  const exit = Promise.withResolvers();
  const lease = controllerLease(() => exit.promise);
  const records = new Map();
  let globallyReleased = false;
  const bridge = await openBridge(channel, { close() {} }, { read: () => ({ generation, messages: [] }), close() {} }, key, {
    partner: { ...lease,
      // 只接纳当前实例的动作，原票据始终以控制实例和工作号查找。
      call(method, id, context, skill) {
        const work = `${context.controller}/${id}`;
        const record = records.get(work);
        if (record) return record;
        if (method !== 'partner.begin' || !lease.owns(context.controller) || globallyReleased) throw new Error('controller not available');
        const receipt = { ...context, skill, exited: false };
        records.set(work, receipt);
        return receipt;
      },
    },
    // 为显式接管保留桥接级释放，普通退出走独立方法。
    releaseControl() { globallyReleased = true; return { generation, order: 0, phase: 'released' }; },
    // 夹具中的动作资源由可控的原退出承诺负责。
    close() {},
    // 桥接级接管始终独立于普通控制权交接。
    execution() { return { mode: 'partner', state: globallyReleased ? 'released' : 'ready' }; },
  });
  // 通过实际命名管道提交一帧，明确断开的拒绝结果。
  // 步骤：连接 → 写入完整帧 → 读取回执或无回包拒绝。
  async function query(method, arguments_, id = method) {
    const socket = createConnection(pipeName(channel));
    let response = '';
    await once(socket, 'connect');
    socket.end(JSON.stringify({ version: 1, id, key, method, ...arguments_ }) + '\n');
    try { for await (const chunk of socket) response += chunk; }
    catch (error) { if (error.code !== 'ECONNRESET') throw error; }
    return response ? JSON.parse(response) : null;
  }
  // 查看属于该实例的已读状态和控制就绪。
  const listen = controller => query('partner.listen', { generation, after: 0, controller });
  // 用相同短号检验新旧实例的工作回执各自独立。
  const action = (method, controller) => query(method, { context: { generation, order: 1, controller },
    skill: 'move', arguments: { place: 'player' } }, 'work.1');
  // 即使游戏代次未知，也只能请求归还本实例的角色持权。
  const detach = controller => query('control.detach', { generation: '', controller });
  try {
    assert.ok((await query('chat.read', { generation, after: 0 })).inbox);
    assert.equal(await action('partner.begin', b), null);
    assert.equal((await listen(a)).inbox.execution.state, 'ready');
    assert.equal((await listen(b)).inbox.execution.state, 'busy');
    assert.equal((await action('partner.begin', a)).action.exited, false);
    assert.equal(await action('partner.begin', b), null);
    assert.equal((await detach(b)).control.phase, 'released');
    assert.equal((await listen(a)).inbox.execution.state, 'ready');

    assert.equal((await detach(a)).control.phase, 'requested');
    assert.equal((await listen(b)).inbox.execution.state, 'busy');
    const oldReceipt = records.get(`${a}/work.1`);
    oldReceipt.exited = true;
    oldReceipt.result = { status: 'interrupted' };
    exit.resolve();
    assert.equal((await listen(b)).inbox.execution.state, 'ready');
    assert.equal((await action('partner.begin', b)).action.controller, b);
    assert.deepEqual((await action('partner.read', a)).action, oldReceipt);
    assert.equal((await detach(a)).control.phase, 'released');
    assert.equal((await listen(a)).inbox.execution.state, 'busy');
    assert.equal((await listen(b)).inbox.execution.state, 'ready');
    t.diagnostic('A released after work.1 exited; B admitted under the same short ID; late A detach left B ready');

    assert.equal(await query('control.stop', { generation }), null);
    assert.equal((await listen(b)).inbox.execution.state, 'ready');
    assert.equal((await query('control.stop', { generation, scope: 'role' })).control.phase, 'released');
    assert.equal((await listen(b)).inbox.execution.state, 'released');
  } finally { exit.resolve(); await bridge.close(); }
});

// 异常退出依靠续期失效回收，也必须先证实原动作真的退出。
test('an expired controller is replaced only after its work drains', async () => {
  let now = 0, drainCount = 0;
  const exit = Promise.withResolvers();
  const lease = controllerLease(() => { ++drainCount; return exit.promise; }, () => now);
  assert.equal(lease.claim('a'), true);
  now = 7000;
  assert.equal(lease.claim('a'), true);
  now = 8100;
  assert.equal(lease.owns('a'), true);
  now = 15100;
  assert.equal(lease.claim('b'), false);
  assert.equal(lease.owns('a'), false);
  await Promise.resolve();
  assert.equal(drainCount, 1);
  exit.resolve();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(lease.claim('b'), true);
  assert.equal(lease.detach('a'), 'released');
  assert.equal(lease.owns('b'), true);
});
