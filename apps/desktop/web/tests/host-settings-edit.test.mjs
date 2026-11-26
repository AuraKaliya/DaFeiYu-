import { test } from 'node:test';
import assert from 'node:assert/strict';
import { useHostSettings } from '../src/host-settings.ts';
import { useHostSettingEdit } from '../src/host-settings-edit.ts';

// 由同窗读口签发配置，原提交回执和当前配置分别送达。
function fixture(memory=new Map(),host='one',allow=true) {
  const sent=[]; let now=0;
  const post=q=>sent.push(q), settings=useHostSettings(post,()=>now);
  const edit=useHostSettingEdit(settings,post,{getItem:k=>memory.get(k),setItem:(k,v)=>memory.set(k,v)},()=>now);
  const sample=(revision='1',timeout=17000,canSave=allow)=>{
    settings.refresh(); settings.receive({version:'1',event:'settings.snapshot',owner:'host',instance:host,request:sent.findLast(q=>q.type==='settings.read').request,
      state:'ready',revision,appliedRevision:'1',pendingRestart:timeout!==17000,canSave,fields:[
        {id:'provider',type:'string',access_mode:'read_only',source:'startup',value:'deepseek',appliedValue:'deepseek'},
        {id:'timeout_ms',type:'integer',access_mode:'writable',source:'startup',min:1,max:600000,value:timeout,appliedValue:17000}]});
    edit.tick();
  };
  const result=(state,q=sent.at(-1),more={})=>edit.receive({version:'1',event:'settings.result',owner:'host',request:q.request,
    instance:q.instance,submission:q.submission,state,revision:'2',appliedRevision:'1',pendingRestart:true,...more});
  settings.context(host,true);sample();
  return {edit,settings,sent,sample,result,advance:ms=>{now+=ms;edit.tick();}};
}

// 权限与范围来自宿主，冲突前的草稿必须由用户确认后再绑定新修订。
// 步骤：核对只读和范围 → 保存旧基准遇冲突 → 保留输入 → 显式按新修订保存。
test('a current writable field and the original edit revision are required before saving',()=>{
  const f=fixture(),readonly=fixture(new Map(),'one',false);
  readonly.edit.input('25000');readonly.edit.save();assert.equal(readonly.sent.some(q=>q.type==='settings.save'),false);
  for(const text of ['','0','600001','1.5','1e4','-1','17000']){
    f.edit.input(text);assert.equal(f.edit.canSave(),false);
  }
  f.edit.input('19000');f.edit.save();const old=f.sent.at(-1);f.edit.save();
  assert.deepEqual(old.changes,{timeout_ms:19000});assert.equal(old.revision,'1');assert.equal(f.sent.at(-1),old);
  f.result('conflict',old);f.sample('2',18000);
  assert.equal(f.edit.draft.value.text,'19000');assert.equal(f.edit.draft.value.revision,'1');assert.equal(f.edit.canSave(),false);
  f.edit.rebase();f.edit.save();const fresh=f.sent.at(-1);
  assert.equal(fresh.revision,'2');assert.notEqual(fresh.submission,old.submission);
  f.result('saved',fresh,{savedRevision:'3',savedValue:19001});assert.equal(f.edit.receipt.value.state,'sending');
  f.result('saved',fresh,{savedRevision:'3',savedValue:19000});assert.equal(f.edit.receipt.value.savedRevision,'3');
  f.sample('3',19000);assert.equal(f.settings.view.value.reading.appliedTimeout,17000);
});

// 丢失保存应答后仅查询已持有的提交号，页面和后台重启都不会重放写入。
// 步骤：丢弃初次回包 → 重载按原号查询 → 拒绝错号和错值 → 在新后台核实原保存。
test('lost save results and the original submission survive a page or resident restart',()=>{
  const store=new Map(),f=fixture(store);
  f.edit.input('24000');f.edit.save();const first=f.sent.at(-1);f.advance(3600);f.advance(1300);
  assert.equal(f.sent.at(-1).type,'settings.receipt');assert.equal(f.sent.at(-1).submission,first.submission);
  const page=fixture(store),lookup=page.sent.at(-1);
  assert.equal(lookup.type,'settings.receipt');assert.equal(lookup.submission,first.submission);
  page.result('saved',lookup,{submission:'wrong',savedRevision:'2',savedValue:24000});assert.equal(page.edit.receipt.value.state,'uncertain');
  page.result('saved',lookup,{savedRevision:'2',savedValue:24000});
  const restart=fixture(store,'replacement'),again=restart.sent.at(-1);
  assert.equal(again.type,'settings.receipt');assert.equal(again.instance,'replacement');assert.equal(again.submission,first.submission);
  restart.result('saved',again,{savedRevision:'2',savedValue:24000,appliedRevision:'2',pendingRestart:false});
  assert.equal(restart.edit.receipt.value.savedRevision,'2');
  assert.equal([...page.sent,...restart.sent].some(q=>q.type==='settings.save'),false);
});
