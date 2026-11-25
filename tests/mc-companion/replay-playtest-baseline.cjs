const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const { createHash } = require('node:crypto');
const { readFileSync } = require('node:fs');
const { join, resolve } = require('node:path');

const fingerprints = {
  'host/trace/events.jsonl': 'd86b7eac49056ad0f8dc5e3d15a6da242ee37d9d8361d26f73387cd11ef1c24c',
  'host/game-actions.jsonl': '717666dbda2807797951f11de792cc68b87a9b721113a1b11da2cf5f855189bf',
  'console.log': '4cb6f5cb493ef4aeb1546c94313c367555acec92c5b1a8a6a8d45d90b02ca47e',
};

// 只接受本局三份未改写的原始文件，回放材料由调用者在本地提供。
function readEvidence(root, file) {
  const bytes = readFileSync(join(root, file));
  assert.equal(createHash('sha256').update(bytes).digest('hex'), fingerprints[file], '证据指纹不符 ' + file);
  return bytes.toString('utf8').trimEnd().split(/\r?\n/);
}

// 从实际送模消息取出 MC 上下文，与工具定义使用同一次模型请求。
function context(request) {
  return JSON.parse(request.input.messages.findLast(message => message.role === 'user').content);
}

// 核对发生在游戏边界上的现象，包括回放所替代的工作和票据。
// 步骤：对照工作及原话现场 → 按事项核对重复呈现 → 用控制台与 Trace 复核确认等待。
function verifyGame(trace, actions, consoleLog) {
  const follow = actions.find(row => row.skill === 'follow');
  const stopped = trace.find(row => row.kind === 'tool.exited' && row.toolId === 'mc.companion/stop_motion');
  assert.deepEqual([follow.work, follow.result.progress.phase, follow.result.status, JSON.parse(stopped.output).work],
    ['1.67', 'in_range', 'interrupted', '1.67']);
  const observation = trace.find(row => row.kind === 'tool.exited' && row.run === 6 &&
    row.toolId === 'mc.companion/observe' && JSON.parse(row.arguments).message === 5);
  const pointed = JSON.parse(observation.output);
  const original = context(trace.find(row => row.kind === 'model.prepared' && row.run === 6)).events[0];
  assert.equal(pointed.anchor.message, original.sequence);
  assert.deepEqual(pointed.anchor.position, original.sceneAtMessage.position);
  assert.notDeepEqual(pointed.anchor.position, pointed.player);

  const ask = trace.find(row => row.kind === 'ask.opened');
  const asks = actions.filter(row => row.skill === 'reply' && row.work.startsWith('ask.'));
  assert.deepEqual(asks.map(row => row.result.text), [ask.item, ask.ask].map(id => `${ask.question} 回复 答复 ${id} <回答>`));
  assert.equal(asks.every(row => row.exited && row.result.sent), true);
  assert.ok(asks.every(row => consoleLog.some(line => line.includes('/tell ') && line.endsWith(row.result.text))));

  const presented = trace.find(row => row.category === 'mc.confirmation.presentation').body;
  const decision = trace.find(row => row.category === 'mc.confirmation.decision');
  const received = trace.find(row => row.category === 'mc.partner.event' && row.body.event.sequence === 7);
  const prompt = trace.find(row => row.kind === 'model.prepared' && row.run === 11);
  assert.deepEqual([decision.body.id, decision.body.status, decision.body.skill], [presented.id, 'expired', 'collect']);
  assert.ok(decision.seq < prompt.seq);
  assert.deepEqual(context(prompt).events, [received.body.event]);
  const collect = trace.find(row => row.kind === 'tool.exited' && row.toolId === 'mc.companion/collect');
  assert.equal(JSON.parse(collect.output).status, 'blocked');
  const confirmation = actions.find(row => row.work === presented.bridgeWork);
  const reply = actions.find(row => row.order === 7 && row.skill === 'reply');
  const visible = consoleLog.findIndex(line => line.includes('/tell ') && line.endsWith(confirmation.result.text));
  const said = consoleLog.findIndex(line => line.includes(']: [Not Secure] <') && line.endsWith(received.body.event.text));
  const answered = consoleLog.findIndex(line => line.includes('/tell ') && line.endsWith(reply.result.text));
  assert.ok(visible >= 0 && visible < said && said < answered);
  assert.deepEqual([visible, said, answered].map(index => consoleLog[index].slice(1, 9)), ['17:36:17', '17:36:28', '17:36:45']);
  assert.ok(actions.every(row => !['collect', 'place'].includes(row.skill)));
}

// 对实际模型请求和工具退出做固定基线断言，区分选错名称和目录欠缺。
// 步骤：核对任务修订 → 检查不可用调用的本轮目录 → 检查后续回合才出现的放置工具。
function verifyModel(trace) {
  const requests = trace.filter(row => row.kind === 'model.prepared');
  const exits = trace.filter(row => row.kind === 'tool.exited');
  const requestFor = exit => requests.find(row => row.run === exit.run && row.step === exit.step && row.attempt === exit.attempt);
  const updates = exits.filter(row => row.toolId === 'mc.companion/task_update');
  assert.deepEqual(updates.map(row => row.run), [3, 3, 11]);
  for (const row of updates) {
    const result = JSON.parse(row.output);
    assert.deepEqual([JSON.parse(row.arguments).revision, context(requestFor(row)).task.revision, result.status, result.task.revision],
      [1, 0, 'stale', 0]);
  }
  assert.ok(requests.filter(row => row.step === 1).every(row => context(row).task.goal === ''));
  const unavailable = exits.filter(row => row.error === 'unavailable');
  assert.deepEqual(unavailable.map(row => row.run), [7, 12]);
  const [badAlias, missingMove] = unavailable.map(row => requestFor(row).input.tools.map(tool => tool.name));
  assert.ok(badAlias.some(name => name.endsWith('_ask_player')) && !badAlias.includes(unavailable[0].name));
  assert.ok(!missingMove.some(name => name.endsWith('_move')));
  assert.ok(requests.filter(row => row.run === 12).every(row => !row.input.tools.some(tool => tool.name.endsWith('_place'))));
  assert.ok(requests.find(row => row.run === 13).input.tools.some(tool => tool.name.endsWith('_place')));
  assert.ok(exits.every(row => !row.name.endsWith('_place')));
}

// 使用现有回放器逐轮复用录制模型输出，逐一核对实际送模和工具退出。
// 步骤：确定回合范围及预期失败 → 运行本地回放 → 核对每轮计数和结算。
function replay(exe, traceFile, trace) {
  const runs = trace.filter(row => row.kind === 'run.exited');
  assert.deepEqual(runs.map(row => row.run), Array.from({ length: 14 }, (_, index) => index + 1));
  assert.deepEqual(runs.filter(row => row.result !== 'succeeded').map(row =>
    [row.run, row.result, row.diagnostic.data.timeline.stop_reason]), [[10, 'failed', 'source_denied']]);
  for (const { session, run, result } of runs) {
    const rows = trace.filter(row => row.session === session && row.run === run);
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

// 固定历史版本的 Trace、游戏回执和控制台，逐一验证本局证据和离线回放。
// 步骤：加载并核对原件 → 验证故障和游戏效果 → 重跑本局全部模型回合。
function main() {
  const [exe, root] = process.argv.slice(2);
  if (!exe || !root) throw new Error('用法 node replay-playtest-baseline.cjs <dafeiyu-cli.exe> <run-4VDQ2M 目录>');
  const traceFile = 'host/trace/events.jsonl';
  const trace = readEvidence(root, traceFile).map(JSON.parse);
  const actions = readEvidence(root, 'host/game-actions.jsonl').map(JSON.parse);
  const consoleLog = readEvidence(root, 'console.log');
  assert.equal(trace.length, 406);
  assert.ok(trace.every((row, index) => row.seq === index + 1));
  assert.equal(trace.at(-1).kind, 'trace.closed');
  assert.equal(actions.length, 29);
  verifyModel(trace);
  verifyGame(trace, actions, consoleLog);
  replay(resolve(exe), resolve(root, traceFile), trace);
  console.log('run-4VDQ2M 的 Trace、游戏回执与控制台基线已核对');
}

main();
