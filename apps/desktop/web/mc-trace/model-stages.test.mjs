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

// 同一规则可出现在旧布局或新版职责段，只有第五段正文是新布局的人格。
// 步骤：还原双换行格式和尾随协议 → 检查跨段隔离 → 保持残缺记录走原文展示。
test('五段规则的 MC 指引来自职责段，人格只取末段，旧日志仍可读', () => {
  const headers = ['【职责与信息来源 v1】','【授权与行动边界 v1】','【判断与结算 v1】','【输出与投递 v1】','【人格与风格 v1】'];
  const guidance = {flowGuidance:{x:'观察'},quoted:'行内【人格与风格 v1】'};
  for (const title of ['Minecraft 同场规则','Minecraft 委托规则','Minecraft 动作复核']) {
    const source = [headers[0],'原场景事实来自插件。',title,JSON.stringify(guidance),'本步宿主附加协议',
      headers[1],'按本次授权',headers[2],'核对实效',headers[3],'投递给玩家',headers[4],'按角色说话。','保留第二行。'];
    for (const eol of ['\n','\r\n']) {
      assert.deepEqual(systemParts(source.join(eol)),{title,persona:['按角色说话。','保留第二行。'].join(eol),guidance});
      assert.deepEqual(systemParts(['旧人格',title,JSON.stringify(guidance,null,2)].join(eol)),{title,persona:'旧人格',guidance});
    }
    assert.equal(systemParts(source.filter(line=>line!==headers[2]).join('\n')),undefined);
    assert.equal(systemParts(source.filter(line=>line!==title).concat(title,JSON.stringify(guidance)).join('\n')),undefined);
    assert.equal(systemParts(source.map(line=>line===headers[1]?headers[3]:line).join('\n')),undefined);
  }
});
