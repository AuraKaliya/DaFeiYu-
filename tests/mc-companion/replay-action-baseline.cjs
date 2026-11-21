const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const { createHash } = require('node:crypto');
const { readFileSync } = require('node:fs');
const { resolve } = require('node:path');

const files = ['host/trace/events.jsonl', 'host/game-actions.jsonl', 'console.log'];
const fingerprints = {
  'run-L5G5AB': [
    'c575c3356ac9e8deaa8f12086eef807eb6bd1c6c185b8d9ecca37f926b83722b',
    '2ea54995223f936748c89d65887984848f15df91046b2ec5cddec02d0e8e49a9',
    'b41bfd5d56e35056118b0409c21d7d75c30ea282f713b4f559bb0486f7c33420'
  ],
  'run-LdgI0H': [
    '2354d82d3656338dc22505280bde1f63693e1b7530bd603c2e41d4555dd68046',
    'f2ebef5d8f9bcd9ac5216869bce487ee779d4b06fcdb734dacb84b1f3714fcdc',
    '356e1b2a23d10714d2ca659a6d1b0c4dd276068d726c552103654d58fb10d801'
  ],
  'run-FX9QKV': [
    '69a900abfc0060dd7d1627fbb09349559c379d94ce6adddc5213082564bbc061',
    '7f83b1beba02469a0eecd3eea77cd4e3017b1452a228d48527155679522b198d',
    '2410fa24c0256862481606583ccd3677026a4f433c0a6bbd11af39405a89652f'
  ],
  'run-rg8RdZ': [
    'a57c1e7f9631f7cc1ca2970c1678fb849407e5aafeff948073307bb1f965b293',
    '9e0939140718e49b0f35f403f5186c65dde70cf8112b249a228097df1d9d2e56',
    '8d1fba3401ec971afe02ee5cc92fbf0b2d11cac862d555a3129bf69844475cb2'
  ]
};

// 从本地原件加载模型、游戏和控制台证据并核对完整性。
// 步骤：校验指纹 → 解析日志 → 核对顺序、闭合及玩家原话。
function loadRun(directory, name, thinking, player = 'Aura') {
  const paths = files.map(file => resolve(directory, file));
  const [trace, game, paper] = paths.map((file, index) => {
    const bytes = readFileSync(file);
    assert.equal(createHash('sha256').update(bytes).digest('hex'), fingerprints[name][index], file);
    const lines = bytes.toString('utf8').trimEnd().split(/\r?\n/);
    return index === 2 ? lines : lines.map(JSON.parse);
  });
  assert.ok(trace.every((row, index) => row.seq === index + 1));
  assert.equal(trace.at(-1).kind, 'trace.closed');
  const prepared = trace.filter(row => row.kind === 'model.prepared');
  assert.ok(prepared.length > 0 && prepared.every(row => row.thinking === thinking));
  const chat = trace.filter(row => row.category === 'mc.partner.event' && row.body.event.type === 'player.message');
  const speaker = `<${player}> `;
  assert.deepEqual(chat.map(row => row.body.event.text), paper.filter(line => line.includes(speaker))
    .map(line => line.split(speaker)[1]));
  return { trace, game, path: paths[0] };
}

// 核对关闭思考时的两次实效和未执行的转身。
function checkOff({ trace, game }) {
  assert.deepEqual(game.filter(row => row.skill === 'collect' && row.result.broken === 1)
    .map(row => row.result.changes[0].x), [4, 3]);
  assert.equal(game.find(row => row.skill === 'follow').result.status, 'interrupted');
  assert.equal(trace.find(row => row.run === 5 && row.kind === 'run.exited').diagnostic.data.timeline.stop_reason, 'model_limit');
  assert.equal(trace.filter(row => row.run === 11 && row.kind === 'tool.exited').length, 0);
}

// 固化开启思考时的采集、恢复与放置故障，及最终世界实效。
// 步骤：比对采掘退出与出队 → 检查跟随未恢复 → 核对目标被清空后的放置。
function checkOn({ trace, game }) {
  const second = game.find(row => row.work === 'queued.2282.2');
  assert.deepEqual([second.result.status, second.result.changes[0].before, second.result.inventoryDelta],
    ['done', 'grass_block', { dirt: 1 }]);
  const cancelled = trace.filter(row => row.category === 'mc.partner.queue' && row.body.status === 'prerequisite_failed');
  assert.deepEqual(cancelled.map(row => row.body.works), [['queued.2282.3'], ['queued.2765.2']]);
  assert.equal(game.find(row => row.work === '1.761').result.status, 'interrupted');
  assert.ok(cancelled.flatMap(row => row.body.works).every(work => !game.some(row => row.work === work)));

  const ended = trace.filter(row => row.kind === 'run.exited' && row.result === 'cancelled');
  assert.deepEqual(ended.map(row => [row.run, row.diagnostic.data.timeline.stop_reason]), [[13, 'deadline'], [14, 'deadline']]);
  const wall = trace.filter(row => row.run === 15 && row.kind === 'tool.exited');
  assert.ok(wall.some(row => row.output && JSON.parse(row.output).reason === 'target cells outside the original survey'));
  const blocked = wall.find(row => row.name.endsWith('_place'));
  assert.equal(JSON.parse(blocked.output).placementAttempted, false);
  const cleared = wall.find(row => row.name.endsWith('_task_update') && row.succeeded &&
    Object.keys(JSON.parse(row.arguments).target ?? { unbound: true }).length === 0);
  assert.ok(blocked.seq < cleared.seq);
  assert.equal(game.find(row => row.work === 'queued.2960.1').result.inventoryDelta.dirt, 1);
  assert.deepEqual(game.filter(row => row.skill === 'place').map(row => row.result.materialAfter), [3, 2, 1, 0]);
}

// 用原 Trace 里的固定模型输出回放可由单回合重现的路径。
// 步骤：启动本地回放器 → 检查退出码 → 核对回放的模型与工具计数。
function replay(exe, evidence, session, runs) {
  for (const run of runs) {
    const rows = evidence.trace.filter(row => row.run === run);
    const model = rows.filter(row => row.kind === 'model.submitted').length;
    const tools = rows.filter(row => row.kind === 'tool.exited').length;
    const child = spawnSync(exe, ['--replay-trace', evidence.path, String(session), String(run)],
      { encoding: 'utf8', windowsHide: true, timeout: 30_000 });
    if (child.error) throw child.error;
    assert.equal(child.status, 0, child.stderr || child.stdout);
    assert.equal(child.stdout.trim(), `Replay matched session ${session} run ${run}: ${model} model requests, ${tools} tool exits, succeeded`);
  }
}

// 对照同一块泥土和四格缺口的两局实效，保留外部事件的真实顺序。
// 步骤：核对实际入包及跟随工作号 → 核对补墙缺失的回执 → 检查录制回合的控制边界。
function checkFollowAndWall(off, on) {
  for (const evidence of [off, on]) {
    const collected = evidence.game.filter(row => row.skill === 'collect' && row.result.status === 'done');
    assert.deepEqual(collected.map(row => [row.result.changes, row.result.inventoryDelta]), [
      [[{ x: -7, y: -60, z: -2, before: 'dirt', after: 'air' }], { dirt: 1 }]
    ]);
    assert.ok(!evidence.game.some(row => row.skill === 'place'));
  }
  const resumed = off.trace.find(row => row.category === 'mc.partner.motion' &&
    row.body.action.work === '1.134' && row.body.action.segment === 2 && row.body.status === 'accepted');
  assert.equal(resumed.body.action.bridgeWork, '1.134.resume.2');
  assert.equal(off.game.find(row => row.work === '1.134.resume.2').result.status, 'blocked');
  assert.ok(off.trace.some(row => row.category === 'mc.confirmation.execution' &&
    row.body.execution === 'skipped' && row.body.status === 'expired'));

  const failures = on.game.filter(row => row.skill === 'collect' && row.result.status === 'blocked');
  assert.deepEqual(failures.map(row => row.result.reason), Array(2).fill('未读到符合取材范围的地面'));
  assert.equal(on.game.find(row => row.work === '1.45').result.status, 'blocked');
  assert.ok(!on.trace.some(row => row.body?.action?.work === '1.45' && row.body.action.segment > 1));
  assert.ok(!on.game.some(row => (row.order === 5 || row.order === 6) && row.skill !== 'observe'));
  const stops = on.trace.filter(row => row.kind === 'run.exited' && [3, 9, 10].includes(row.run));
  assert.deepEqual(stops.map(row => row.diagnostic.data.timeline.stop_reason), ['model_limit', 'source_denied', 'source_denied']);
  const observed = on.game.find(row => row.work === '1.550').result.observation.samples[0].verified;
  assert.deepEqual(observed.filter(cell => cell.block === 'air').map(cell => [cell.x, cell.y, cell.z]),
    [[2, -60, -5], [3, -60, -5], [2, -59, -5], [3, -59, -5]]);
}

// 复核指定一对实机证据及固定回合，实时控制分支由原始事件顺序取证。
function main() {
  const [exe, root, thinkingOn] = process.argv.slice(2);
  if (!exe || !root) throw new Error('用法 node replay-action-baseline.cjs <dafeiyu.exe> <evidence 目录或 run-FX9QKV 目录> [run-rg8RdZ 目录]');
  const executable = resolve(exe);
  if (thinkingOn) {
    const off = loadRun(root, 'run-FX9QKV', false, 'DfyVisitor');
    const on = loadRun(thinkingOn, 'run-rg8RdZ', true, 'DfyVisitor');
    checkFollowAndWall(off, on);
    replay(executable, off, 54, [3, 11, 13]);
    replay(executable, on, 54, [4, 5, 6]);
    console.log('两局跟随与补四格基线及 6 个固定响应回合已核对');
    return;
  }
  const off = loadRun(resolve(root, 'run-L5G5AB'), 'run-L5G5AB', false);
  const on = loadRun(resolve(root, 'run-LdgI0H'), 'run-LdgI0H', true);
  checkOff(off);
  checkOn(on);
  replay(executable, off, 53, [6, 7, 10, 11]);
  replay(executable, on, 53, [6, 7, 9, 10, 11, 12, 15]);
  console.log('两局动作基线与 11 个固定响应回合已核对');
}

main();
