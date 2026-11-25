import assert from 'node:assert/strict';
import { test } from 'node:test';
import { decodedObject, id, parseJson, readLine, scanFile } from './trace-file.ts';

// 生成与宿主格式一致、默认以换行结束的记录。
function traceRow(seq, kind, data = '') {
  return `{"schema":1,"seq":${seq},"kind":"${kind}"${data}}\r\n`;
}

// 核对跨分块中文和长行在字节索引处仍能还原原文。
test('按字节读取跨块中文并保留原生整数', async () => {
  const body = '格'.repeat(30000);
  const row = traceRow(2, 'model.prepared', `,"session":9007199254740999,"content":"${body}"`);
  const file = new Blob([traceRow(1, 'trace.open'), row, traceRow(3, 'trace.closed')]);
  const refs = [];
  const state = await scanFile(file, 'trace', (value, ref) => {
    refs.push(ref);
    if (ref.line !== 2) return;
    assert.equal(id(value.session), '9007199254740999');
    assert.equal(value.content, body);
  });
  assert.deepEqual(state, { lines: 3, state: 'closed' });
  assert.equal(await readLine(file, refs[1]), row.trimEnd());
});

// 核对完整前缀、未写完尾行以及继续读会误关联的坏记录。
test('截尾保留完整记录，坏序号终止索引', async () => {
  const open = traceRow(1, 'trace.open');
  const prepared = traceRow(2, 'model.prepared');
  const visited = [];
  const tail = await scanFile(new Blob([open, prepared, '{"schema":1']), 'trace', value => visited.push(value.kind));
  assert.deepEqual(visited, ['trace.open', 'model.prepared']);
  assert.deepEqual(tail, { lines: 2, state: 'open', issue: '第 3 行尚未结束' });
  const gap = await scanFile(new Blob([open, traceRow(3, 'trace.closed')]), 'trace', () => {});
  assert.equal(gap.state, 'invalid');
  assert.match(gap.issue, /第 2 行.*序号/);
  const badAfterClose = await scanFile(
    new Blob([open, traceRow(2, 'trace.closed'), traceRow(3, 'run.exited')]), 'trace', () => {},
  );
  assert.match(badAfterClose.issue, /结束标记后/);
});

// 核对游戏侧无宿主头尾的文件以及字符串正文可以独立读取。
test('游戏回执支持独立行索引，只有对象才作为业务正文', async () => {
  const file = new Blob(['{"work":"1.9","exited":true}\n']);
  const visited = [];
  assert.deepEqual(await scanFile(file, 'game', (value, ref) => visited.push([value.work, ref.stream])),
    { lines: 1, state: 'complete' });
  assert.deepEqual(visited, [['1.9', 'game']]);
  assert.deepEqual(decodedObject('{"status":"done"}'), { status: 'done' });
  assert.equal(decodedObject('not JSON'), undefined);
  assert.equal(parseJson('9007199254740992'), '9007199254740992');
  assert.equal(parseJson('9007199254740993'), '9007199254740993');
});

// 原生主键和导入快照中的子对象都必须保留唯一字段。
test('重复键和超深记录不会进入文件索引', async () => {
  const open = traceRow(1, 'trace.open');
  const duplicates = traceRow(2, 'model.prepared', ',"input":{"seq":1,"\\u0073eq":2}');
  const deep = traceRow(2, 'model.prepared', ',"input":' + '['.repeat(52) + '0' + ']'.repeat(52));
  for (const [row, issue] of [[duplicates, /重复字段/], [deep, /嵌套超过限制/]]) {
    const observed = [];
    const result = await scanFile(new Blob([open, row]), 'trace', value => observed.push(value.kind));
    assert.deepEqual(observed, ['trace.open']);
    assert.equal(result.state, 'invalid');
    assert.match(result.issue, issue);
  }
  const escaped = { a: [']{}"\\aba', { key: '1' }, { key: '2' }] };
  assert.deepEqual(parseJson(JSON.stringify(escaped)), escaped);
});
