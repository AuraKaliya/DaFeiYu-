import { test } from 'node:test';
import assert from 'node:assert/strict';
import { useHostSettings } from '../src/host-settings.ts';

const fields = [
  {id:'provider',type:'string',access_mode:'read_only',source:'startup',value:'deepseek',appliedValue:'deepseek'},
  {id:'timeout_ms',type:'integer',access_mode:'writable',source:'user',min:1,max:600000,value:20000,appliedValue:17000},
];
// 固定时钟和请求号只向当前窗口返回合成快照。
function fixture() {
  const sent=[]; let now=0;
  const settings=useHostSettings(q=>sent.push(q),()=>now);
  const send=(extra={}, request=sent.at(-1))=>settings.receive({event:'settings.snapshot',version:'1',request:request.request,owner:'host',
    instance:'original',state:'ready',revision:'90071992547409931',appliedRevision:'1',pendingRestart:true,canSave:false,fields,...extra});
  const advance=ms=>{now+=ms;settings.tick();};
  return {settings,sent,send,advance};
}
// 旧后台不能填入新窗口，保存值与已采用值按各自的修订展示。
// 步骤：确认当前字段 → 遇到不可用或错误权限 → 切换后台并忽略迟到回复。
test('owner, saved configuration and effective values remain bound to the original snapshot',()=>{
  const f=fixture(), current=()=>f.settings.view.value;
  f.settings.context('original',true);
  assert.deepEqual({...f.sent[0],request:'r'},{type:'settings.read',owner:'host',instance:'original',version:'1',request:'r'});
  f.send({fields:[...fields,{id:'private',value:'unpublished data'}]});
  assert.equal(current().reading.timeout,20000); assert.equal(current().reading.appliedTimeout,17000);
  assert.equal(current().reading.appliedRevision,'1'); assert.equal(current().reading.revision,'90071992547409931');
  assert.equal(current().reading.canSave,false); assert.equal(JSON.stringify(current()).includes('unpublished data'),false);
  f.settings.refresh(); f.send({state:'stale',fields:[]}); assert.equal(current().status,'stale');
  for(const invalid of [{state:'unavailable',fields:[]},{owner:'plugin'},{fields:[fields[0],{...fields[1],value:0}]},
      {fields:[fields[0],{...fields[1],access_mode:'read_only'}]},{fields:[fields[0],fields[0],fields[1]]}]) {
    f.settings.refresh(); f.send(invalid);
    assert.equal(current().status,'unavailable'); assert.equal(current().reading.timeout,20000);
  }
  f.settings.refresh(); const old=f.sent.at(-1); f.settings.context('new',true);
  f.send({},old); assert.equal(current().reading,undefined);
  f.send({instance:'new',canSave:true}); assert.equal(current().reading.instance,'new');
  f.settings.context('',true); assert.equal(current().status,'stale');
  const count=f.sent.length; f.advance(10000); assert.equal(f.sent.length,count);
});

// 首次失败和超时有独立反馈，关闭设置之后不再自动读取。
test('unavailable and timed-out configuration never looks like a configured empty host',()=>{
  const f=fixture(); f.settings.context('original',true); f.advance(3600);
  assert.equal(f.settings.view.value.status,'unavailable'); assert.equal(f.settings.view.value.reading,undefined);
  f.advance(2000); f.send({instance:'wrong'}); assert.equal(f.settings.view.value.status,'unavailable');
  f.settings.refresh(); f.send(); f.settings.context('original',false); const count=f.sent.length;
  f.advance(9000); assert.equal(f.sent.length,count); assert.equal(f.settings.view.value.status,'stale');
});
