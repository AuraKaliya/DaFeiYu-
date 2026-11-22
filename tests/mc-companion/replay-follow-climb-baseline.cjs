const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const { createHash } = require('node:crypto');
const { readFileSync } = require('node:fs');
const { join, resolve } = require('node:path');

const fingerprints = {
  'host/trace/events.jsonl': 'eeaff4b36b2bed9dff94c0bba17ec5c70f2ca5a3f9ef239475de3aea43c9a62f',
  'host/game-actions.jsonl': '908fca69b188cb02c6f2626b81eb2402495b1ac3ffc80a96a6117bdf0e81bee9',
  'console.log': '5263ebc50a97b5f51bf429460bec9d4a1adc311f4160319c26fd3932977615be',
};

// 校验原始证据后读取本局对应的文件。
function evidence(root, name) {
  const bytes = readFileSync(join(root, name));
  assert.equal(createHash('sha256').update(bytes).digest('hex'), fingerprints[name], name);
  return bytes.toString('utf8').trimEnd().split(/\r?\n/);
}

// 核对实际掉队的工作号、距离和原号退出，以及没有投递的自主文本。
// 步骤：核对实际动作账本 → 对照移动进度 → 核对出水、受阻与最终停止。
function verifyFollow(trace, actions) {
  const follows = actions.filter(row => row.skill === 'follow');
  assert.deepEqual(follows.map(({ work, result }) => [work, result.status, result.progress.distance]), [
    ['1.240', 'blocked', 7.1], ['1.267', 'blocked', 7.9], ['1.276', 'blocked', 13.9], ['1.315', 'interrupted', 8.3],
  ]);
  assert.equal(follows[0].result.navigation.crossings, 1);
  const progress = trace.filter(row => row.category === 'mc.partner.motion' && row.body.action.work === '1.240');
  for (const [distance, phase] of [[11.2, 'following'], [3.4, 'in_range'], [10.8, 'following']])
    assert.ok(progress.some(row => row.body.progress?.distance === distance && row.body.status === phase));
  const runs = trace.filter(row => row.kind === 'model.prepared' && row.step === 1);
  for (const [run, work] of [[3, '1.240'], [4, '1.267'], [5, '1.276'], [7, '1.315']]) {
    const context = JSON.parse(runs.find(row => row.run === run).input.messages.at(-1).content);
    assert.equal(context.trigger, 'motion.finished');
    assert.equal(context.workSettlement.receipts[0].bridgeWork, work);
    assert.ok(trace.find(row => row.run === run && row.kind === 'run.exited').answer);
    assert.ok(!trace.some(row => row.category === 'mc.partner.action' && row.body.request === run && row.body.skill === 'reply'));
  }
}

// 核对攀高计划拒绝、虚假等待与追问未投递的原始行为。
// 步骤：匹配玩家原话 → 检查送模目录和提交拓扑 → 核对拒绝、发言及零实际动作。
function verifyClimb(trace, actions, consoleLog) {
  const events = trace.filter(row => row.category === 'mc.partner.event').map(row => row.body.event);
  assert.deepEqual(events.filter(event => event.text).map(event => event.text),
    ['大肥鱼，跟着我', '你上来', '能上来吗', '你可以垫上来']);
  const requests = trace.filter(row => row.kind === 'model.prepared');
  assert.equal(requests.length, 29);
  assert.ok(requests.every(row => row.thinking === false));
  const exits = trace.filter(row => row.kind === 'tool.exited');
  const plans = exits.filter(row => row.toolId === 'mc.companion/queue_actions');
  assert.deepEqual(plans.map(row => row.run), [8, 8, 9]);
  assert.deepEqual(JSON.parse(plans[0].arguments).steps.map(node => [node.skill, node.after ?? []]),
    [['place', []], ['move', ['p1']], ['place', ['p2']], ['move', ['p3']]]);
  for (const rejected of plans) {
    const output = JSON.parse(rejected.output);
    assert.deepEqual([output.status, output.reason, output.skill], ['invalid_plan', 'unsupported_plan_skill', 'place']);
  }
  const queue = requests.find(row => row.run === 8 && row.step === 4).input.tools.find(tool => tool.name.endsWith('_queue_actions'));
  const nodes = JSON.parse(queue.parameters).properties.steps.items.properties.skill.enum;
  assert.ok(nodes.includes('place') && nodes.includes('move'));
  assert.equal(trace.find(row => row.kind === 'run.exited' && row.run === 8).diagnostic.data.timeline.stop_reason, 'work_pending');
  const ask = exits.find(row => row.toolId === 'mc.companion/ask_player');
  assert.deepEqual([ask.run, JSON.parse(ask.output).status], [9, 'already_requested']);
  assert.ok(actions.every(row => !['move', 'place'].includes(row.skill)));
  const spoken = actions.filter(row => row.skill === 'reply');
  assert.equal(spoken.length, 4);
  assert.ok(spoken.every(row => row.exited && row.result.sent &&
    consoleLog.some(line => line.includes('/tell DfyVisitor ') && line.endsWith(row.result.text))));
}

// 逐轮用已有离线回放入口重用录制的模型响应及工具返回。
// 步骤：选择原会话、回合 → 执行离线重放 → 对照请求数和退出结果。
function replay(exe, traceFile, trace) {
  for (const { run, session, result } of trace.filter(row => row.kind === 'run.exited')) {
    const rows = trace.filter(row => row.run === run && row.session === session);
    const models = rows.filter(row => row.kind === 'model.submitted').length;
    const tools = rows.filter(row => row.kind === 'tool.exited').length;
    const child = spawnSync(exe, ['--replay-trace', traceFile, String(session), String(run)],
      { encoding: 'utf8', windowsHide: true, timeout: 30000 });
    if (child.error) throw child.error;
    assert.equal(child.status, 0, child.stderr || child.stdout);
    assert.equal(child.stdout.trim(), `Replay matched session ${session} run ${run}: ${models} model requests, ${tools} tool exits, ${result}`);
    console.log(child.stdout.trim());
  }
}

// 固定 run-CA2yMm 的两条回归基线与真实日志的关联。
// 步骤：加载证据 → 核对掉队与攀高 → 重放所有历史回合。
function main() {
  const [exe, root] = process.argv.slice(2);
  if (!exe || !root) throw new Error('用法 node replay-follow-climb-baseline.cjs <dafeiyu.exe> <run-CA2yMm 目录>');
  const traceFile = 'host/trace/events.jsonl';
  const trace = evidence(root, traceFile).map(JSON.parse);
  const actions = evidence(root, 'host/game-actions.jsonl').map(JSON.parse);
  assert.equal(trace.length, 243);
  assert.equal(trace.at(-1).kind, 'trace.closed');
  verifyFollow(trace, actions);
  verifyClimb(trace, actions, evidence(root, 'console.log'));
  replay(resolve(exe), resolve(root, traceFile), trace);
}

main();
