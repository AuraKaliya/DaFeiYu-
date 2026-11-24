const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const test = require('node:test');
const { firstUsefulFeedback, measurePlayerFeedback } = require('./player-feedback.cjs');

const useful = '两个缺口都看到了，还差两块泥土。';

// 用同一原话的受控玩家时钟核对首条有用反馈的边界以及缺失端点。
test('按原话和实际收件判定五秒边界', () => {
  const sample = { sentAtMs: 100, replies: [{ text: '收到', atMs: 110 }, { text: '左边还是右边？', atMs: 130 }] };
  for (const [elapsedMs, status] of [[4999, 'on_time'], [5000, 'on_time'], [5001, 'late']]) {
    const replies = [...sample.replies, { text: useful, atMs: 100 + elapsedMs }, { text: useful, atMs: 7000 }];
    assert.deepEqual(firstUsefulFeedback({ ...sample, replies }, useful), { status, elapsedMs });
  }
  assert.deepEqual(firstUsefulFeedback({ ...sample, observedThroughMs: 5200 }, useful), { status: 'late' });
  assert.deepEqual(firstUsefulFeedback({ ...sample, observedThroughMs: 4200 }, useful), { status: 'unconfirmed' });
  assert.deepEqual(firstUsefulFeedback({ replies: [] }, useful), { status: 'unconfirmed' });
  assert.deepEqual(firstUsefulFeedback(sample, useful), { status: 'unconfirmed' });
});

// 用固定事件次序核对发话起点、实际对端、断线截止和监听资源释放。
test('协议玩家只采纳本次窗口的伙伴私聊', async () => {
  let atMs = 20;
  const player = new EventEmitter();
  player.chat = text => {
    assert.equal(text, '都补');
    atMs += 10;
    player.emit('whisper', 'OtherPlayer', useful);
    player.emit('whisper', 'DfyCompanion', '收到');
  };
  let sample;
  await assert.rejects(measurePlayerFeedback(player, '都补', useful, async () => {
    atMs = 4100;
    player.emit('end');
    atMs = 7000;
    player.emit('whisper', 'DfyCompanion', useful);
    throw new Error('采样中断');
  }, result => { sample = result; }, () => atMs), /采样中断/);
  assert.deepEqual([sample.sentAtMs, sample.observedThroughMs, sample.status], [20, 4100, 'unconfirmed']);
  assert.deepEqual(sample.replies, [{ text: '收到', atMs: 30 }]);
  assert.equal(player.listenerCount('whisper') + player.listenerCount('end'), 0);
});
