import assert from 'node:assert/strict';
import { test } from 'node:test';
import { comparePrompts, milliseconds, modelStages, stageTiming, systemParts } from './model-stages.ts';
import { indexHost } from './rounds.ts';

// 验证模型尝试、同名候选与宿主成功但游戏拒绝的业务状态彼此独立。
test('按模型尝试和候选号关联，耗时只取已知区间', async () => {
  const base = { session: 57, run: 2, source: 'plugin', step: 1, attempt: 1 };
  const rows = [
    { kind: 'trace.open' },
    { kind: 'subagent.accepted', plugin: 'mc.companion', request: 1 },
    { kind: 'model.submitted', submittedNs: 0 },
    { kind: 'model.exited', timeNs: 8000000, startedNs: 2000000, finishedNs: 7000000 },
    { kind: 'model.rewritten' },
    { kind: 'tool.candidate', candidate: 3, callId: 'reused' },
    { kind: 'tool.candidate', candidate: 4, callId: 'reused' },
    { kind: 'tool.admitted', candidate: 4, invocation: 8 },
    { kind: 'tool.exited', candidate: 4, invocation: 8, result: 'succeeded',
      output: '{"status":"invalid_plan","reason":"expected_one_pointed_dig"}' },
    { kind: 'model.prepared', attempt: 2 },
    { kind: 'trace.closed' },
  ];
  const file = new Blob(rows.map((row, offset) => JSON.stringify({ ...base, schema: 1, seq: offset + 1, ...row }) + '\n'));
  const doc = await indexHost(file);
  const [first, second] = modelStages(doc.rounds[0]);
  assert.deepEqual(stageTiming(first), { queue: 2, service: 5, readback: 1 });
  assert.equal(first.rewritten.kind, 'model.rewritten');
  assert.deepEqual(first.tools.map(tool => [tool.key, tool.exited?.result, tool.exited?.business]),
    [['3', undefined, undefined], ['4', 'succeeded', 'invalid_plan']]);
  assert.equal(second.key, '1/2');
  assert.equal(stageTiming(second).service, undefined);
  assert.equal(milliseconds('9007199254740999', '9007199254740999'), 0);
  assert.equal(milliseconds('2', '1'), undefined);
});

// 核对仅根据录制的领域标题和送模对象展示提示词变化。
test('已知系统段可拆解，工具范围与 MC 事实按原快照对照', () => {
  assert.deepEqual(systemParts('人格\nMinecraft 同场规则\n{"flowGuidance":{"x":"观察"}}'), {
    title: 'Minecraft 同场规则', persona: '人格', guidance: { flowGuidance: { x: '观察' } },
  });
  assert.equal(systemParts('历史系统正文'), undefined);
  const before = { tools: [{ name: 'observe' }, { name: 'reply', description: 'A' }] };
  const after = { tools: [{ name: 'reply', description: 'B' }, { name: 'place' }], messages: [
    { role: 'user', content: '{"generation":"world","trigger":"game.event","events":[]}' },
  ] };
  assert.deepEqual(comparePrompts(before, after), {
    added: ['place'], removed: ['observe'], changed: ['reply'], facts: ['generation', 'trigger', 'events'],
    messages: [{ position: 1, role: 'user', change: 'added' }],
  });
});
