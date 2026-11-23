import assert from 'node:assert/strict';
import { test } from 'node:test';
import { indexDocument } from './mc-links.ts';

// 保存准备、原生接纳与游戏回执分别到达的宿主顺序。
function captured(rows) {
  return new Blob([{ kind: 'trace.open' }, ...rows, { kind: 'trace.closed' }]
    .map((row, index) => JSON.stringify({ schema: 1, seq: index + 1, ...row }) + '\n'));
}

// 生成支持跨页关联的已提交插件回合。
function turn(run, trigger, extras = {}) {
  return [
    { kind: 'model.prepared', source: 'plugin', session: 57, run,
      input: { messages: [{ role: 'user', content: JSON.stringify({ generation: 'world', trigger, events: [], ...extras }) }] } },
    { kind: 'subagent.accepted', plugin: 'mc.companion', session: 57, run, request: run + 10 },
  ];
}

// 以宿主信箱的原有域标签保留业务证据。
function receipt(type, body) {
  return { kind: 'mail.received', category: 'mc.partner.' + type, body };
}

// 验证后继回合可以引用较晚才写入的实际收据，归属保持在原工作页。
test('计划、原问题和游戏回执跨回合按身份连接', async () => {
  const action = { generation: 'world', session: 57, request: 12, bridgeWork: 'plan.90.1', skill: 'fill_target',
    result: { status: 'done', changes: [{ before: 'air', after: 'dirt' }] } };
  const source = captured([
    ...turn(1, 'game.event', { events: [{ sequence: 20, type: 'player.message', text: '补墙' }] }),
    { kind: 'tool.admitted', source: 'plugin', session: 57, run: 1, invocation: 77 },
    receipt('question', { generation: 'world', item: 77, status: 'presented', order: 20 }),
    ...turn(2, 'game.event', { events: [{ sequence: 21, type: 'player.message', text: '都补' }],
      pendingQuestion: { item: 77 } }),
    receipt('question', { generation: 'world', item: 77, status: 'resolved', sourceMessage: 20, answerMessage: 21 }),
    { kind: 'tool.admitted', source: 'plugin', session: 57, run: 2, invocation: 90 },
    receipt('plan', { status: 'queued', plan: { id: 90, nodes: [{ work: 'plan.90.1' }] } }),
    ...turn(3, 'plan.finished', { taskPlan: { id: 90, origin: { sequence: 20 } },
      planSettlements: [{ plan: 90, nodes: [{ work: 'plan.90.1' }] }] }),
    receipt('action', action),
  ]);
  const game = new Blob([JSON.stringify({ generation: 'world', work: 'plan.90.1', skill: 'fill_target', exited: true,
    result: action.result }) + '\n', '{"generation":"other","work":"plan.90.1","exited":true}\n']);
  const doc = await indexDocument(source, game);
  const answer = doc.evidence.get('57/2');
  const finish = doc.evidence.get('57/3');
  assert.ok(answer.entries.some(row => row.kind === 'action' && row.changes === 1));
  assert.ok(answer.entries.some(row => row.ref.stream === 'game'));
  assert.ok(answer.references.some(row => row.status === 'resolved'));
  assert.ok(answer.links.some(link => link.round === '57/1' && link.label === '原追问'));
  assert.ok(finish.links.some(link => link.round === '57/1' && link.label === '原委托'));
  assert.ok(finish.links.some(link => link.round === '57/2' && link.label === '原游戏工作'));
  assert.ok(finish.references.some(row => row.kind === 'action'));
  assert.equal(doc.unlinked.length, 1);
  assert.equal(doc.unlinked[0].generation, 'other');
});
