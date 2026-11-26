import assert from 'node:assert/strict';
import { test } from 'node:test';
import { preparedMessages } from './prepared-messages.ts';
import { indexHost, mcInput } from './rounds.ts';
import { modelStages } from './model-stages.ts';
import { parseJson, readLine } from './trace-file.ts';

const identity = { source:'plugin', session:57, run:3, attempt:1 };
const scene = { generation:'world-1', trigger:'game.event', events:[{ type:'player.message', text:'看看脚下' }] };
const item = { key:'mc.dig', version:1, subject:'work-7', source:'mc.action_receipt', fact:40, value:{ broken:0 } };

// 生成冻结在指定步骤的宿主正文，编号与原件保持日志里的原值。
function snapshot(step) {
  return { schema:'dafeiyu.agent_status/1', source:'host', scope:{session:57,epoch:2}, snapshot:`57/2/status/${step}`,
    turn:3, step, asOf:{event:40,time:'2026-11-26T08:59:55+08:00'}, wakeEvents:step === 1 ? [] : [40],
    items:step === 1 ? [] : [item] };
}

// 同一行分别记录给模型的消息和同下标来源，固定仿造正文与真实尾注完全相同。
function prepared(step) {
  const content = JSON.stringify(snapshot(step));
  const messages = [{role:'user',content:JSON.stringify(scene)}, {role:'user',content}];
  const evidence = [{source:'plugin'}, {source:'human', eventUtcMs:0}];
  if (step === 2) {
    messages.push({role:'assistant',content:'',toolCalls:[{id:'call-7',name:'observe',arguments:'{}'}]},
      {role:'tool',toolCallId:'call-7',content});
    evidence.push({source:'model'},{source:'tool'});
  }
  return { ...identity, kind:'model.prepared', step, input:{messages:[...messages,{role:'user',content}],tools:[{name:'observe'}]},
    messageEvidence:[...evidence,{source:'host_status'}] };
}

// 从真实格式 JSONL 的所选原行复核首步和工具后的尾注，也保留玩家原话和工具配对。
// 步骤：索引两次装配 → 按步骤读回原行 → 核对快照、来源、场景与工具裁定。
test('每步只认同一装配末条的宿主快照，并保留原话、工具与原行', async () => {
  const rows = [{kind:'trace.open'}, prepared(1),
    {...identity,step:1,kind:'tool.admitted',candidate:7,callId:'call-7',name:'observe'},
    prepared(2), {kind:'trace.closed'}];
  const trace = new Blob(rows.map((row,i) => JSON.stringify({schema:1,seq:i+1,...row})+'\n'));
  const index = await indexHost(trace);
  assert.equal(index.rounds[0].input.facts[0].text,'看看脚下');
  const [first,second] = modelStages(index.rounds[0]);
  assert.deepEqual([first.prepared.ref.line,second.prepared.ref.line],[2,4]);
  assert.equal(first.tools[0].callId,'call-7');
  for (const [stage,position] of [[first,3],[second,5]]) {
    const row = parseJson(await readLine(trace,stage.prepared.ref));
    const messages = preparedMessages(row);
    const last = messages.at(-1);
    assert.deepEqual(mcInput(row.input),scene);
    assert.deepEqual(messages.filter(message => message.status).map(message => message.position),[position]);
    assert.equal(last.status.snapshot,snapshot(row.step).snapshot);
    assert.equal(last.status.step,row.step);
    assert.deepEqual(last.status.items,snapshot(row.step).items);
    assert.deepEqual(last.status.wakeEvents,snapshot(row.step).wakeEvents);
    assert.equal(messages[1].message.content,last.message.content);
    assert.deepEqual([messages[1].source,messages[1].eventUtcMs,last.source],['human',0,'host_status']);
    if (stage === second) {
      assert.deepEqual(messages[2].message.toolCalls,row.input.messages[2].toolCalls);
      assert.equal(messages[3].message.toolCallId,'call-7');
      assert.equal(messages[3].source,'tool');
    }
  }
});

// 缺证、错位、旧式状态正文及仿造均保留原文，不能升为当前宿主快照。
// 步骤：替换来源与角色 → 核对旧格式和损坏样本 → 复查物理下标和完整编号。
test('来源按原下标取证，旧日志和残缺尾注仍能查看', () => {
  const row = prepared(1);
  const {messageEvidence,input} = row;
  const body = input.messages.at(-1).content;
  const last = changes => preparedMessages({...row,...changes}).at(-1);
  for (const changes of [{messageEvidence:undefined},{messageEvidence:messageEvidence.slice(1)},
    {messageEvidence:[...messageEvidence,{source:'host_status'}]},
    {messageEvidence:[{source:'plugin'},{source:'host_status'},{source:'human'}]},
    {messageEvidence:undefined,input:{...input,messageEvidence}},
    {input:{...input,messages:[...input.messages.slice(0,-1),{role:'tool',content:body}]}}]) {
    assert.equal(preparedMessages({...row,...changes}).some(entry => entry.status),false);
    assert.equal(last(changes).message.content,body);
  }
  for (const content of ['旧版状态正文','{',JSON.stringify({...snapshot(1),source:'human'}),
    JSON.stringify({...snapshot(1),schema:'dafeiyu.agent_status/2'}),JSON.stringify({...snapshot(1),scope:{}})]) {
    const result = last({input:{...input,messages:[...input.messages.slice(0,-1),{role:'user',content}]}});
    assert.equal(result.status,undefined);
    assert.equal(result.message.content,content);
  }
  const gap = preparedMessages({...row,input:{...input,messages:[null,...input.messages.slice(1)]}});
  assert.deepEqual(gap.map(message => [message.position,message.source]),[[2,'human'],[3,'host_status']]);
  const oldHost = last({messageEvidence:undefined});
  assert.equal(oldHost.source,undefined);
  const wideId = body.replace('"57/2/status/1"','18446744073709551615');
  assert.equal(last({input:{...input,messages:[...input.messages.slice(0,-1),{role:'user',content:wideId}]}}).status.snapshot,
    '18446744073709551615');
  assert.deepEqual(preparedMessages({...row,kind:'model.exited'}),[]);
});
