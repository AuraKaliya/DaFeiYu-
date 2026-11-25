import assert from 'node:assert/strict';
import { test } from 'node:test';
import { indexGame, projectMc } from './mc-records.ts';

// 提供真实信箱事件的索引坐标，格式也允许读取旧版的字符串正文。
const entry = { ref: { stream: 'trace', line: 2, start: 4, end: 12 }, kind: 'mail.received', time: '990' };

// 核对嵌套动作、计划和送达回执的身份及状态。
test('投影保留 MC 动作、计划和送达各自的原始身份', () => {
  const action = projectMc({ category: 'mc.partner.motion', body: JSON.stringify({
    status: 'accepted', action: { session: 57, request: 2, invocation: 12, bridgeWork: '1.12',
      work: 'follow-original', generation: 'world', skill: 'follow' },
  }) }, entry);
  assert.deepEqual([action.session, action.request, action.work, action.follow, action.status, action.time],
    ['57', '2', '1.12', 'follow-original', 'accepted', '990']);
  const plan = projectMc({ category: 'mc.partner.plan',
    body: { status: 'queued', plan: { id: 21, nodes: [{ work: 'plan.21.1' }] } } }, entry);
  assert.deepEqual([plan.plan, plan.workItems], ['21', ['plan.21.1']]);
  const reply = projectMc({ category: 'mc.partner.action',
    body: { bridgeWork: '1.13', effect: 3, result: { sent: false, delivery: 'unknown', text: '尝试发言' } } }, entry);
  assert.deepEqual([reply.delivery, reply.effect], ['unknown', 'unknown']);
});

// 核对只有独立游戏证据时仍保留效果，并且时间字段不继承宿主时钟。
test('桥接退出可以独立读回逐格效果', async () => {
  const file = new Blob(['{"work":"plan.1.2","generation":"g","skill":"fill_target","exited":true,',
    '"result":{"status":"blocked","changes":[{"before":"air","after":"dirt"}]}}\n']);
  const game = await indexGame(file);
  assert.equal(game.source.state, 'complete');
  const [receipt] = game.records;
  assert.deepEqual([receipt.work, receipt.status, receipt.changes, receipt.time], ['plan.1.2', 'blocked', 1, undefined]);
});
