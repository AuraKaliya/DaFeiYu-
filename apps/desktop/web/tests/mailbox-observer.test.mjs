import { test } from 'node:test';
import assert from 'node:assert/strict';
import { useMailboxObserver } from '../src/mailbox-observer.ts';

const sources = [{ id: '11', label: '书房' }, { id: '12', label: '测试插件' }];

// 固定本窗时钟与后台回包，每次读取仍经过正式观察器。
function fixture() {
  const sent = [];
  let time = 0;
  const panel = useMailboxObserver(request => sent.push(request), () => time);
  const latest = type => sent.findLast(item => item.type === type);
  const reply = (request, fields) => panel.receive({ version: '1', request: request.request, instance: 'resident-1', ...fields });
  const directory = (members = sources, extra = {}, request = latest('mailbox.list')) =>
    reply(request, { event: 'mailbox.directory', revision: '3', sources: members, ...extra });
  const snapshot = (messages, extra = {}, request = latest('mailbox.read')) =>
    reply(request, { event: 'mailbox.snapshot', source: request.source, revision: '6', state: 'ready', messages, ...extra });
  const advance = ms => { time += ms; panel.tick(); };
  panel.context(true,true);
  return { panel, sent, latest, reply, directory, snapshot, advance };
}

// 收起、重复打开、切换来源和新目录都不交付系统收件或回信命令。
test('one room entry reads only the selected original source', () => {
  const f = fixture(), panel = f.panel;
  assert.deepEqual(Object.keys(f.sent[0]).sort(),['request','type','version']);
  f.directory();
  panel.visibility(true); f.directory();
  panel.select('11');
  const previous = f.latest('mailbox.read');
  assert.deepEqual([previous.instance,previous.source],['resident-1','11']);
  const publicRecord = { id:'letter',category:'test',state:'received',payload:'私有字段' };
  panel.select('12');
  f.snapshot([publicRecord],{},previous);
  assert.equal(panel.view.value.snapshot,undefined);
  f.snapshot([]);
  assert.deepEqual(panel.view.value.snapshot.messages,[]);
  panel.select('11'); f.snapshot([publicRecord]);
  const oldRead = f.latest('mailbox.read');
  assert.equal(JSON.stringify(panel.view.value).includes('私有字段'),false);
  assert.equal(panel.view.value.snapshot.messages[0].state,'received');
  panel.visibility(false); panel.visibility(true);
  assert.notEqual(f.latest('mailbox.read').request,oldRead.request);
  f.snapshot([publicRecord]);
  f.directory([{id:'13',label:'书房'}],{revision:'4'});
  assert.equal(panel.view.value.detailStatus,'stale');
  assert.equal(panel.view.value.snapshot,undefined);
  assert.equal(panel.view.value.source,'11');
  panel.select('13');
  f.snapshot([],{state:'stale'});
  assert.equal(panel.view.value.detailStatus,'stale');
  assert.deepEqual(new Set(f.sent.map(request=>request.type)),new Set(['mailbox.list','mailbox.read']));
});

// 未响应、读取失败和后台换代分别保持最近快照或撤销旧来源，晚到回包无权复活旧记录。
test('timeout and reconnect never present a failed read as an empty source', () => {
  const f = fixture(), panel = f.panel;
  f.directory(); panel.visibility(true); f.directory(); panel.select('12');
  f.snapshot([{id:'1',category:'demo',state:'pending'}]);
  f.advance(1600);
  const delayed = f.latest('mailbox.read');
  f.reply(delayed,{event:'management.error',reason:'internal diagnostic'});
  assert.equal(panel.view.value.detailStatus,'unavailable');
  assert.equal(panel.view.value.snapshot.messages[0].id,'1');
  panel.context(false,true);
  assert.equal(panel.view.value.directoryStatus,'stale');
  const old = f.latest('mailbox.list');
  panel.context(true,true);
  assert.equal(f.latest('mailbox.list').instance,undefined);
  f.directory([],{},old);
  assert.equal(panel.view.value.directoryStatus,'loading');
  f.directory(sources,{instance:'resident-2',revision:'1'});
  assert.equal(panel.view.value.source,'');
  assert.match(panel.view.value.selectionNotice,/后台实例已更换/);
  assert.equal(panel.view.value.snapshot,undefined);
  panel.select('12');
  f.snapshot([],{instance:'resident-2',revision:'1'});
  assert.equal(panel.view.value.detailStatus,'ready');
  f.snapshot([{id:'late',category:'demo',state:'pending'}],{},delayed);
  assert.equal(panel.view.value.snapshot.instance,'resident-2');
  assert.deepEqual(panel.view.value.snapshot.messages,[]);
  f.advance(1600); f.advance(3600);
  assert.equal(panel.view.value.directoryStatus,'unavailable');
  assert.equal(panel.view.value.detailStatus,'unavailable');
});

// 两窗用自己的请求号和选择，不同实例、缺项和重复身份的回包都不能覆盖有效投影。
test('windows validate independent replies before publishing any letters', () => {
  const a = fixture(), b = fixture();
  b.directory([],{},a.latest('mailbox.list'));
  assert.equal(b.panel.view.value.directoryStatus,'loading');
  a.directory(); b.directory();
  a.panel.visibility(true); a.directory(); a.panel.select('12');
  b.panel.visibility(true); b.directory(); b.panel.select('11');
  a.snapshot([{id:'x',category:'demo',state:'pending'}]);
  b.snapshot([]);
  a.advance(1600); a.directory();
  a.snapshot([{id:'x',category:'demo',state:'pending'},{id:'x',category:'demo',state:'received'}]);
  assert.equal(a.panel.view.value.detailStatus,'unavailable');
  assert.equal(a.panel.view.value.snapshot.messages[0].state,'pending');
  assert.equal(b.panel.view.value.detailStatus,'ready');
  a.panel.context(true,false);
  const count = a.sent.length;
  a.advance(10000);
  assert.equal(a.sent.length,count);
});

// 原信箱撤销或同号换代时清除旧详情，目录的失效响应不会变成成功的空目录。
test('stale snapshots and source generations cannot reuse an earlier binding', () => {
  const f = fixture(), panel = f.panel;
  const member = { id:'11',label:'同名来源',kind:'plugin',generation:'1',plugin:'sample' };
  f.directory([member]); panel.visibility(true); f.directory([member]); panel.select(member.id);
  f.snapshot([{id:'old',category:'demo',state:'pending'}]);
  f.advance(1550);
  f.snapshot([],{state:'stale',revision:'0'});
  assert.equal(panel.view.value.detailStatus,'stale');
  assert.equal(panel.view.value.snapshot,undefined);
  f.directory([{...member,generation:'2'}],{revision:'4'});
  assert.equal(panel.view.value.source,'');
  assert.match(panel.view.value.selectionNotice,/重新选择/);
  panel.select(member.id); f.snapshot([],{revision:'1'});
  assert.equal(panel.view.value.detailStatus,'ready');
  f.advance(1550); f.directory([],{state:'stale',instance:'resident-2',revision:'0'});
  assert.equal(panel.view.value.directoryStatus,'stale');
  assert.equal(panel.view.value.snapshot,undefined);
  f.advance(1550);
  assert.equal(f.latest('mailbox.list').instance,undefined);
  f.directory([],{state:'ready',instance:'resident-2',revision:'1'});
  assert.equal(panel.view.value.directoryStatus,'ready');
  assert.equal(panel.view.value.directory.sources.length,0);
  assert.equal(panel.view.value.source,'');
  f.advance(1550); f.directory([member],{instance:'resident-2',revision:'2'});
  panel.select(member.id);
  f.snapshot([],{instance:'resident-3',revision:'1',state:'stale'});
  assert.equal(panel.view.value.detailStatus,'stale');
  assert.equal(panel.view.value.directoryStatus,'stale');
  assert.equal(f.latest('mailbox.list').instance,undefined);
});

// 同一信件分别核实实际入箱及取件时间，原地址不符时保留最近一次可信读数。
test('receipts belong to the selected letter and display only confirmed public fields', () => {
  const f = fixture(), panel = f.panel;
  f.directory(); panel.visibility(true); f.directory(); panel.select('11');
  const receipt = { mailbox:'84',sequence:'1',state:'accepted',acceptedUtcMs:1730000000000,acceptedRevision:'5' };
  const row = { id:'84/1',category:'test.notice',categoryVersion:1,direction:'toHost',state:'pending',receipt,payload:'私有正文' };
  f.snapshot([row]);
  let letter = panel.view.value.snapshot.messages[0];
  assert.equal(letter.direction,'toHost');
  assert.equal(letter.categoryVersion,1);
  assert.deepEqual(letter.receipts,[
    {label:'原信箱',value:'84'}, {label:'投递序号',value:'1'}, {label:'入箱时间',utcMs:1730000000000}, {label:'入箱修订',value:'5'},
  ]);
  const collected = {...receipt,state:'received',receivedUtcMs:1730000000200,receivedRevision:'7',privateNote:'私有回执'};
  f.advance(1550); f.directory();
  f.snapshot([{...row,state:'received',receipt:collected}],{revision:'7'});
  letter = panel.view.value.snapshot.messages[0];
  assert.deepEqual(letter.receipts.slice(-2),[{label:'取件时间',utcMs:1730000000200},{label:'取件修订',value:'7'}]);
  assert.equal(JSON.stringify(letter).includes('私有'),false);
  f.advance(1550); f.directory();
  f.snapshot([{...row,state:'received',receipt:{...collected,sequence:'2'}}],{revision:'8'});
  assert.equal(panel.view.value.detailStatus,'unavailable');
  assert.equal(panel.view.value.snapshot.revision,'7');
});

// 只要地址水位显示早期回执已裁剪，当前投影就不能被当成完整历史。
test('retention coverage follows the original mailbox watermarks', () => {
  const f = fixture(), panel = f.panel;
  f.directory(); panel.visibility(true); f.directory(); panel.select('11');
  f.snapshot([],{mailboxes:[{accepted:'0',firstRetained:'0'}]});
  assert.equal(panel.view.value.snapshot.historyLimited,false);
  f.advance(1550); f.directory();
  f.snapshot([],{mailboxes:[{accepted:'150',firstRetained:'0'}]});
  assert.equal(panel.view.value.snapshot.historyLimited,true);
  f.advance(1550); f.directory();
  f.snapshot([],{mailboxes:[{accepted:'1',firstRetained:'9'}]});
  assert.equal(panel.view.value.detailStatus,'unavailable');
});
