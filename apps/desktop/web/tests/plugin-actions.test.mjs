import { test } from 'node:test';
import assert from 'node:assert/strict';
import { usePluginActions } from '../src/plugin-actions.ts';
import { usePluginDirectory } from '../src/plugin-directory.ts';

const target = { id:'test.one', name:'插件', version:'1', installed:true, loaded:false, state:'installed',
  canLoad:true, canUnload:false, generation:'0' };
const live = { ...target, loaded:true, state:'active', canLoad:false, canUnload:true, generation:'90071992547409931',
  loadedName:'插件', loadedVersion:'1', errorCode:0 };
// 用本窗自己的读数、时钟和存储核对提交及查回，不运行 DLL 或模型。
function fixture(memory = new Map(), instance = 'original') {
  let now = 0;
  const sent = [], post = fields => sent.push(fields);
  const directory = usePluginDirectory(post, () => now);
  const actions = usePluginActions(directory, post, { getItem:key=>memory.get(key), setItem:(key,value)=>memory.set(key,value) }, () => now);
  const sample = (rows=[target], host=instance, revision='42') => {
    directory.refresh();
    directory.receive({version:'1',request:sent.findLast(q=>q.type==='plugins.list').request,event:'plugins.directory',state:'ready',
      instance:host,revision,unavailableCount:0,packages:rows});
    actions.tick();
  };
  const answer = (write, fields={}, query=sent.at(-1)) => actions.receive({version:'1',event:'plugins.operation',instance:write.instance,
    request:query.request,operation:'host-issued-operation',action:write.type.split('.')[1],id:write.id,revision:write.revision,
    targetGeneration:write.generation,generation:write.generation,command:write.command,state:'queued',effect:'unconfirmed',...fields});
  const advance = delay => { now += delay; actions.tick(); };
  directory.context(true,true); sample(); directory.select(target.id);
  return { directory, actions, sample, answer, advance, sent };
}

// 重复点击只生成一份原意图，查回时校验原代次和命令，成功后再读真实目录。
// 步骤：显式加载 → 查原操作并剔除错号 → 核对新代次 → 显式卸载直到原实例退出。
test('one click binds one command and only its original receipt can confirm an effect', () => {
  const f = fixture(), {actions} = f;
  assert.equal(actions.canStart('unload'),false);
  actions.start('load'); const write = f.sent.at(-1);
  actions.start('load');
  assert.equal(f.sent.at(-1),write);
  assert.deepEqual({...write,request:'x'},{type:'plugins.load',version:'1',request:'x',instance:'original',command:'1',id:target.id,revision:'42',generation:'0'});
  f.answer(write,{instance:'other'}); assert.equal(actions.work.value.state,'sending');
  f.answer(write); assert.equal(actions.work.value.state,'queued');
  f.directory.select(target.id); f.advance(400); const query = f.sent.at(-1);
  assert.equal(query.type,'plugins.receipt'); assert.equal(query.operation,actions.work.value.operation);
  for (const fields of [{operation:'wrong'},{id:'wrong'},{targetGeneration:'8'},{command:'2'},{state:'succeeded',effect:'loaded',generation:'0'}]) {
    f.answer(write,fields,query);
    assert.equal(actions.work.value.state,'queued');
  }
  f.answer(write,{state:'succeeded',effect:'loaded',generation:live.generation},query);
  assert.equal(actions.work.value.generation,live.generation);
  assert.equal(actions.canStart('load'),false);
  f.sample([live],'original','43'); actions.start('unload');
  const unload=f.sent.at(-1); assert.equal(unload.command,'2'); assert.equal(unload.generation,live.generation);
  f.answer(unload,{operation:'next'}); f.advance(400);
  f.answer(unload,{operation:'next',state:'pending',effect:'stopping'}); assert.equal(actions.canStart('unload'),false);
  f.advance(400); f.answer(unload,{operation:'next',state:'succeeded',effect:'unloaded'});
  assert.equal(actions.work.value.effect,'unloaded');
  assert.equal(f.sent.filter(q=>['plugins.load','plugins.unload'].includes(q.type)).length,2);
});

// 已受理的工作跨页面重载只查询原号，改选和更换后台不会误投原写入。
// 步骤：保留受理号 → 新文档补读 → 查询失败后恢复 → 在替换后台拒绝沿用。
test('reloaded pages and reconnected windows read the original operation without replay', () => {
  const memory = new Map(), f = fixture(memory);
  f.actions.start('load'); const write=f.sent.at(-1); f.answer(write);
  const restored = fixture(memory);
  assert.equal(restored.sent.at(-1).operation,'host-issued-operation');
  restored.advance(3600); assert.equal(restored.actions.readFailed.value,true);
  restored.advance(1600); restored.answer(write,{state:'busy',effect:'retained',generation:'8'});
  assert.equal(restored.actions.work.value.effect,'retained');
  assert.equal(restored.sent.some(q=>q.type==='plugins.load'),false);
  const other = fixture(memory,'replacement');
  assert.equal(other.actions.work.value,undefined);
  other.actions.start('load'); assert.equal(other.sent.at(-1).command,'1');
  assert.equal(other.sent.some(q=>q.type==='plugins.receipt'),false);
});

// 提交结果未知与明确未分派分开处理，次序错误只修正下一次显式命令。
// 步骤：丢弃首次回包 → 核对无重发 → 归还未分派号 → 由真实期望号校正。
test('loss, local rejection and ordering never trigger an automatic write', () => {
  const f = fixture(); f.actions.start('load'); f.advance(3600);
  assert.equal(f.actions.work.value.state,'uncertain'); f.actions.check(); f.actions.start('load');
  assert.equal(f.sent.filter(q=>q.type==='plugins.load').length,1);
  const g = fixture(); g.actions.start('load'); const rejected = g.sent.at(-1);
  g.actions.receive({version:'1',event:'management.error',request:rejected.request,dispatched:false});
  assert.equal(g.actions.work.value.effect,'notApplied'); g.sample(); g.actions.start('load');
  const ordered = g.sent.at(-1); assert.equal(ordered.command,'1');
  g.answer(ordered,{state:'outOfOrder',effect:'notApplied',expectedCommand:'9'}); g.sample();
  assert.equal(g.sent.filter(q=>q.type==='plugins.load').length,2);
  g.actions.start('load'); assert.equal(g.sent.at(-1).command,'9');
});
