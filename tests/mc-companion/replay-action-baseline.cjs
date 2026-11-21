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
  ]
};

// 从本地原件加载模型、游戏和控制台证据并核对完整性。
// 步骤：校验指纹 → 解析日志 → 核对顺序、闭合及玩家原话。
function loadRun(root, name, thinking) {
  const paths = files.map(file => resolve(root, name, file));
  const [trace, game, paper] = paths.map((file, index) => {
    const bytes = readFileSync(file);
    assert.equal(createHash('sha256').update(bytes).digest('hex'), fingerprints[name][index], file);
    const lines = bytes.toString('utf8').trimEnd().split(/\r?\n/);
    return index === 2 ? lines : lines.map(JSON.parse);
  });
  assert.ok(trace.every((row, index) => row.seq === index + 1));
  assert.equal(trace.at(-1).kind, 'trace.closed');
  assert.ok(trace.filter(row => row.kind === 'model.prepared').every(row => row.thinking === thinking));
  const chat = trace.filter(row => row.category === 'mc.partner.event' && row.body.event.type === 'player.message');
  assert.deepEqual(chat.map(row => row.body.event.text), paper.filter(line => line.includes('<Aura> '))
    .map(line => line.split('<Aura> ')[1]));
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
function replay(exe, evidence, runs) {
  for (const run of runs) {
    const rows = evidence.trace.filter(row => row.run === run);
    const model = rows.filter(row => row.kind === 'model.submitted').length;
    const tools = rows.filter(row => row.kind === 'tool.exited').length;
    const child = spawnSync(exe, ['--replay-trace', evidence.path, '53', String(run)],
      { encoding: 'utf8', windowsHide: true, timeout: 30_000 });
    if (child.error) throw child.error;
    assert.equal(child.status, 0, child.stderr || child.stdout);
    assert.equal(child.stdout.trim(), `Replay matched session 53 run ${run}: ${model} model requests, ${tools} tool exits, succeeded`);
  }
}

// 复核两局故障及固定回合，截止和外部控制分支由原始事件顺序取证。
function main() {
  const [exe, root] = process.argv.slice(2);
  if (!exe || !root) throw new Error('用法 node replay-action-baseline.cjs <dafeiyu.exe> <evidence 目录>');
  const off = loadRun(root, 'run-L5G5AB', false);
  const on = loadRun(root, 'run-LdgI0H', true);
  checkOff(off);
  checkOn(on);
  replay(resolve(exe), off, [6, 7, 10, 11]);
  replay(resolve(exe), on, [6, 7, 9, 10, 11, 12, 15]);
  console.log('两局动作基线与 11 个固定响应回合已核对');
}

main();
