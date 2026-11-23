import assert from 'node:assert/strict';
import { test } from 'node:test';
import { describeTrigger, indexHost } from './rounds.ts';

// 以宿主原格式固定独立回合、投影和事件出现顺序。
export function traceBlob(entries, closed = true) {
  const rows = [{ kind: 'trace.open', utcMs: 0 }, ...entries];
  if (closed) rows.push({ kind: 'trace.closed' });
  return new Blob(rows.map((row, index) => JSON.stringify({ schema: 1, seq: index + 1, timeNs: index * 1e6, ...row }) + '\n'));
}

// 生成一条 MC 事实送模，模拟实际保留的有序消息。
export function prepared(session, run, facts) {
  return { kind: 'model.prepared', source: 'plugin', session, run, step: 1, attempt: 1,
    input: { messages: [{ role: 'user', content: JSON.stringify({ generation: 'world', ...facts }) }], tools: [] } };
}

// 核对已接纳但未送模也有独立入口，且请求号不会跨子会话合并。
test('按宿主会话分页并保留准备、接纳和退出边界', async () => {
  const accept = (session, run = 1, plugin = 'mc.companion') =>
    ({ kind: 'subagent.accepted', session, run, plugin, request: 1 });
  const facts = { trigger: 'game.event', events: [{ type: 'player.message', sequence: 2, text: '补墙' }] };
  const doc = await indexHost(traceBlob([
    prepared(57, 1, facts), { kind: 'model.submitted', source: 'plugin', session: 57, run: 1 }, accept(57),
    prepared(82, 1, { currentEvents: [], newEvents: [], work: { candidate: 200 } }), accept(82),
    accept(57, 2), { kind: 'run.exited', source: 'plugin', session: 57, run: 2, result: 'failed' },
    prepared(90, 1, facts), accept(90, 1, 'other.plugin'),
  ]));
  assert.deepEqual(doc.rounds.map(round => [round.key, round.input?.trigger.label, round.modelCount]), [
    ['57/1', '游戏输入', 1], ['82/1', '在途动作复核', 0], ['57/2', undefined, 0],
  ]);
  assert.equal(doc.rounds[0].accepted.request, doc.rounds[1].accepted.request);
  assert.equal(doc.rounds[2].exit.result, 'failed');
  assert.equal(doc.rounds[0].input.facts[0].text, '补墙');
});

// 保留新触发和缺尾的证据，尚未出现接纳时不擅自标记已接纳。
test('保留未知触发和未接纳的待确认回合', async () => {
  const doc = await indexHost(traceBlob([prepared(51, 1, { trigger: 'future.event', events: [] })], false));
  assert.equal(doc.source.state, 'open');
  assert.equal(doc.rounds[0].accepted, undefined);
  assert.deepEqual(doc.rounds[0].input.trigger,
    { code: 'future.event', label: '触发待确认', category: '触发待确认' });
  assert.equal(describeTrigger({ trigger: 'skills.expanded' }).category, '续行与能力');
});
