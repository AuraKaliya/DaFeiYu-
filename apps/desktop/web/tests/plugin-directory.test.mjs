import { test } from 'node:test';
import assert from 'node:assert/strict';
import { usePluginDirectory } from '../src/plugin-directory.ts';

const installed = {id:'test.one',name:'一号插件',version:'1.2',state:'installed',generation:'0',
  installed:true,loaded:false,canLoad:true,canUnload:false};
const loaded = {id:'test.two',name:'二号插件',version:'2',state:'active',generation:'97',installed:true,
  loaded:true,canLoad:false,canUnload:true,loadedName:'旧二号',loadedVersion:'1',errorCode:0};

// 以可控时钟应答当窗的请求号，不需要操作本机插件。
function fixture() {
  let now = 0;
  const out = [], directory = usePluginDirectory(fields => out.push(fields), () => now);
  const reply = (rows, fields = {}, request = out.at(-1)) => directory.receive({version:'1',request:request.request,
    event:'plugins.directory',instance:'host-a',revision:'90071992547409931',state:'ready',packages:rows,unavailableCount:0,...fields});
  const advance = ms => { now += ms; directory.tick(); };
  directory.context(true,true);
  return { out, directory, reply, advance };
}

// 两窗收取相同目录也各有自己的选中项，重复查看只发目录请求。
// 步骤：交错两窗应答 → 分别选择 → 隐藏再打开后核对保留及移除。
test('each settings window selects from its own validated package directory', () => {
  const a = fixture(), b = fixture();
  assert.deepEqual(Object.keys(a.out[0]).sort(),['request','type','version']);
  b.reply([installed],{},a.out[0]);
  assert.equal(b.directory.view.value.status,'loading');
  a.reply([installed,{...loaded,privatePath:'私有内容'}]); b.reply([installed,loaded]);
  a.directory.select(loaded.id); b.directory.select(installed.id);
  assert.equal(a.directory.view.value.reading.revision,'90071992547409931');
  assert.equal(a.directory.view.value.reading.packages[1].generation,'97');
  assert.equal(JSON.stringify(a.directory.view.value).includes('私有内容'),false);
  a.directory.context(true,false); a.advance(5000);
  assert.equal(a.out.length,1);
  a.directory.context(true,true); a.reply([installed,loaded]);
  assert.equal(a.directory.view.value.selected,loaded.id);
  a.advance(1550); assert.equal(a.out.at(-1).instance,'host-a');
  a.reply([installed],{revision:'90071992547409932'});
  assert.equal(a.directory.view.value.selected,'');
  assert.match(a.directory.view.value.notice,/移出目录/);
  assert.equal(b.directory.view.value.selected,installed.id);
  assert.deepEqual(new Set(a.out.map(x=>x.type)),new Set(['plugins.list']));
});

// 只有完整有效的空列表确认空目录，失联和损坏的快照保留上一次可信读数。
// 步骤：核对空目录 → 制造查询失败并重试 → 拒绝重复包号及迟到回包。
test('empty, unavailable, expired and reconnected readings stay distinct', () => {
  const f = fixture(), {directory} = f;
  f.reply([]); assert.equal(directory.view.value.status,'ready');
  assert.deepEqual(directory.view.value.reading.packages,[]);
  directory.refresh(); f.reply([loaded]); directory.select(loaded.id);
  directory.refresh();
  directory.receive({request:f.out.at(-1).request,version:'1',event:'management.error',reason:'private'});
  assert.equal(directory.view.value.status,'unavailable');
  assert.equal(directory.view.value.reading.packages[0].id,loaded.id);
  directory.refresh(); f.reply([loaded,loaded]); assert.equal(directory.view.value.status,'unavailable');
  directory.refresh(); const late = f.out.at(-1);
  directory.context(false,true); assert.equal(directory.view.value.status,'stale');
  directory.context(true,true); f.reply([],{instance:'unrelated'},late);
  assert.equal(directory.view.value.status,'loading');
  f.reply([loaded]); assert.equal(directory.view.value.selected,loaded.id);
  directory.refresh(); f.advance(3600);
  assert.equal(directory.view.value.status,'unavailable');
  directory.refresh(); f.reply([],{state:'stale',instance:'host-b',revision:'1'});
  assert.equal(directory.view.value.status,'stale'); assert.equal(directory.view.value.selected,'');
  directory.refresh(); assert.equal(f.out.at(-1).instance,undefined);
  f.reply([loaded],{instance:'host-b',revision:'2'}); assert.match(directory.view.value.notice,/后台实例已更换/);
  assert.equal(directory.view.value.selected,'');
});

// 部分发现保留未读数量，运行中的旧清单与已安装版本分别呈现。
// 步骤：读取不完整目录 → 保留真实实例元数据 → 拒绝矛盾的授权及超宽修订。
test('partial scans and installed versus running identities are preserved', () => {
  const f = fixture(), current = () => f.directory.view.value;
  f.reply([],{unavailableCount:2});
  assert.equal(current().reading.unavailableCount,2);
  f.directory.refresh(); f.reply([loaded,{...loaded,id:'disk.removed',installed:false}]);
  assert.equal(current().reading.packages[0].loadedVersion,'1');
  assert.equal(current().reading.packages[0].version,'2');
  assert.equal(current().reading.packages[1].installed,false);
  for (const [rows, fields] of [[[{...loaded,canLoad:true}],{}],[[{...loaded,generation:'0'}],{}],
      [[installed],{unavailableCount:-1}],[[installed],{revision:'18446744073709551616'}]]) {
    f.directory.refresh(); f.reply(rows,fields);
    assert.equal(current().status,'unavailable');
    assert.equal(current().reading.packages.length,2);
  }
});
