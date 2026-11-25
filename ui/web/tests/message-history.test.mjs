import { test } from 'node:test';
import assert from 'node:assert/strict';
import { useMessageHistory } from '../src/message-history.ts';
import { messageTimeline } from '../src/message-timeline.ts';
import { originalReceipt } from '../src/dialogue-recovery.ts';

// 构造带相同原话的不同已提交消息，修订号独立于消息号。
function sample(id, revision = id) {
  return { message: String(id), revision: String(revision), utcMs: 1000 + id, sender: 'user', route: 'send',
    text: '同一句', source: 'desktop', submission: 'test-' + id, receipt: { state: 'accepted' } };
}

// 固定前端桥的收发时序，真实数据协议另由原生集成覆盖。
function fixture() {
  const sent = [];
  const storage = new Map();
  globalThis.sessionStorage = { getItem: key => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, value) };
  globalThis.window = { chrome: { webview: { postMessage: message => sent.push(message) } } };
  return { sent, storage };
}

// 覆盖固定快照翻页与新回执交错、重新订阅、乱序回包以及刷新后恢复到首条。
test('history resumes from applied pages and preserves independent equal messages', () => {
  const { sent } = fixture();
  let view = useMessageHistory();
  const reply = (event, fields, request = sent.at(-1)) => view.receive({ version: '1', event, request: request.request,
    conversation: 'logical', instance: 'resident', watermark: '5', ...fields });
  view.pulse(undefined);
  reply('messages.subscribed', { watermark: '3' });
  reply('messages.page', { through: '3', next: '2', more: true, records: [sample(3), sample(2)] });
  view.pulse('5');
  reply('messages.page', { through: '5', next: '5', more: false, records: [sample(1, 4), sample(4, 5)] });
  view.earlier();
  assert.equal(sent.at(-1).through, '3');
  assert.equal(sent.at(-1).cursor, '2');
  const outdated = sent.at(-1);
  view.disconnect(); view.pulse(undefined);
  reply('messages.page', { through: '3', next: '1', more: false, records: [sample(1)] }, outdated);
  reply('messages.subscribed', {});
  reply('messages.page', { through: '3', next: '1', more: false, records: [sample(1)] });
  assert.deepEqual(view.rows.value.map(row => [row.message, row.revision]), [['1','4'],['2','2'],['3','3'],['4','5']]);
  view = useMessageHistory(); view.pulse(undefined);
  reply('messages.subscribed', {});
  reply('messages.page', { through: '5', next: '3', more: true, records: [sample(4,5),sample(3)] });
  assert.equal(sent.at(-1).cursor, '3');
  reply('messages.page', { through: '5', next: '1', more: false, records: [sample(2),sample(1,4)] });
  assert.equal(view.rows.value.length, 4);
  assert.equal(view.more.value, false);
});

// 同一原生回合的新实例和独立消息不会因正文相同而相互覆盖。
test('timeline associates final deliveries with their original instance and turn', () => {
  const a = sample(1), b = sample(2);
  a.receipt = { state: 'accepted', instance:'old', session:'1', turn:'1', nativeMessage:'0' };
  b.receipt = { ...a.receipt, instance: 'new' };
  const current = { session:'1', turn:'1', queuedMessage:'0', inputs:[] };
  const final = { ...sample(3), sender:'assistant', source:'main', route:'reply', instance:'new', session:'1', turn:'1',
    delivery:'new/1/1/final', receipt:{state:'delivered'} };
  const separate = { ...final, message:'4', delivery:'another-notice' };
  const blocks = messageTimeline([a,b,final,separate],[current],'new');
  assert.equal(blocks.length, 3);
  assert.equal(blocks[0].row.message,'1');
  assert.equal(blocks[1].prompt.message,'2');
  assert.deepEqual(blocks[1].answers.map(row=>row.message),['3']);
  assert.equal(blocks[2].row.message,'4');
});

// 显式发言和最终文本都以实际投递号显示，同文的其他回合及多次正式发言各有自己的消息。
test('explicit and final deliveries render once per stored row, never from a model result alone', () => {
  const turns = [1,2,3].map(i=>({session:'1',turn:String(i),queuedMessage:'0',inputs:[],answer:'同一回答'}));
  const delivered = (id,turn,suffix)=>({ ...sample(id), sender:'assistant', source:'main', route:'reply', text:'同一回答',
    instance:'resident', session:'1', turn:String(turn), delivery:'resident/1/'+turn+'/'+suffix, receipt:{state:'delivered'} });
  const rows = [delivered(1,1,'tool-first'), delivered(2,2,'tool-second'), delivered(3,2,'final'),
    {...delivered(4,3,'final'),receipt:{state:'pending'}}];
  const blocks = messageTimeline(rows,turns,'resident');
  assert.deepEqual(blocks.map(entry=>entry.answers.map(r=>r.message)),[['1'],['2','3'],[]]);
});

// 刷新后的每种持久回执沿用原窗口命令语义，仅肯定接纳才清除未改草稿。
test('recovered submissions agree with native command receipts', () => {
  const fields = { type:'dialogue.append', command:'9', text:'旧补充', submission:'original', sourceUtcMs:80,
    instance:'original-host', session:'3', turn:'8' };
  const row = { ...sample(1), submission:fields.submission, text:fields.text, sourceUtcMs:80, route:'append',
    target:{instance:fields.instance, session:fields.session, turn:fields.turn} };
  for (const [expected, states] of Object.entries({ pending:['pending','dispatching'], accepted:['accepted','waiting','queued','submitted'],
    full:['full','limit'], stale:['stale','closed','removed'], unavailable:['unknown','unavailable'],
    rejected:['cancelled','deadline','failed','rejected','future-unrecognized-state'] })) {
    for (const state of states) assert.equal(originalReceipt(fields,{ ...row, receipt:{state} }),expected,state);
  }
  assert.equal(originalReceipt(fields,{ ...row, sourceUtcMs:81 }), 'conflict');
  assert.equal(originalReceipt(fields,{ ...row, target:{ ...row.target, instance:'replacement' } }), 'conflict');
});

// 手动核对与恢复只读同一原号；断连及迟到的旧文档回复不能表示记录不存在。
test('receipt lookups share a read and preserve the distinction between missing and disconnected', () => {
  const { sent } = fixture();
  const view = useMessageHistory(), results = [];
  view.pulse(undefined);
  const reply = { version:'1', conversation:'logical', instance:'resident', watermark:'1' };
  view.receive({ ...reply, event:'messages.subscribed', request:sent.at(-1).request });
  view.receipt('desktop','test-1', r=>results.push(r));
  const original = sent.at(-1);
  view.receipt('desktop','test-1', r=>results.push(r));
  assert.equal(sent.at(-1),original);
  view.disconnect();
  view.receive({ ...reply,event:'messages.receipt',request:original.request,record:null });
  assert.deepEqual(results,[undefined,undefined]);
  view.pulse(undefined);
  view.receive({ ...reply,event:'messages.subscribed',request:sent.at(-1).request });
  view.receipt('desktop','test-1', r=>results.push(r));
  view.receive({ ...reply,event:'messages.receipt',request:sent.at(-1).request,record:null });
  assert.equal(results.at(-1),null);
});
