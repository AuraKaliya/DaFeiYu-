const assert = require('node:assert/strict');
const { test } = require('node:test');
const { handoffEvidence } = require('../../plugins/mc/bridge/handoff-evidence.cjs');

const id = n => '00000000-0000-0000-0000-' + String(n).padStart(12,'0');
const actor = id(1), owner = id(2), stranger = id(3), mark = { stream: id(4), seq: 0 };

// 复用隔离服实测的整堆和两个单件，后两件在原玩家拾取前合堆。
function evidenceRows() {
  const rows = [];
  const add = (kind, fields) => rows.push({ kind: 'item.'+kind, seq: rows.length+1, stream: mark.stream, tick: 1, ...fields });
  for (const [n, amount] of [[5,64],[6,1],[7,1]]) {
    const item = { material: 'minecraft:dirt', item: id(n), networkId: n, amount };
    add('playerDrop',{ ...item, actor });
    add('added',item);
  }
  add('merge',{ item: id(7), target: id(6), amount: 1, targetAmount: 1, material: 'minecraft:dirt' });
  add('mergeReadback',{ cause: 7, sourceRemaining: 0, targetRemaining: 2 });
  add('pickup',{ tick: 42, material: 'minecraft:dirt', item: id(5), actor: owner, amount: 64, remaining: 0, beforeStock: 0 });
  add('pickup',{ tick: 42, material: 'minecraft:dirt', item: id(6), actor: owner, amount: 2, remaining: 0, beforeStock: 64 });
  add('pickupReadback',{ tick: 43, cause: 9, actor: owner, afterStock: 66, ground: 0 });
  add('pickupReadback',{ tick: 43, cause: 10, actor: owner, afterStock: 66, ground: 0 });
  return rows;
}

// 同刻多堆分别记入原接收者，需同时满足服务端生成、纯来源合堆和库存读回。
test('handoff joins three throws, a pure merge and two pickups in the same server tick', () => {
  const rows = evidenceRows();
  const proof = handoffEvidence(rows,mark,actor,owner,'dirt',66);
  assert.deepEqual([proof.thrown,proof.recipientPicked,proof.otherPicked,proof.throwUnknown,proof.pickupUnknown], [66,66,0,0,0]);
  assert.deepEqual(proof.stacks.map(s => s.count), [64,1,1]);
  assert.deepEqual([proof.afterSeq,proof.throughSeq], [0,12]);
  assert.throws(() => handoffEvidence([...rows,rows.at(-1)],mark,actor,owner,'dirt',66), /乱序/);
  assert.throws(() => handoffEvidence(rows,mark,actor,owner,'dirt',65), /无法分开/);
});

// 缺少生成、混入他来源、库存不符和旁人拾取分别扣在相应的数量边界。
test('handoff preserves uncertainty and attributes a verified pickup by a different player', () => {
  const read = rows => handoffEvidence(rows,mark,actor,owner,'dirt',66);
  const wrongSpawn = evidenceRows();
  wrongSpawn[1].networkId = 88;
  assert.deepEqual([read(wrongSpawn).thrown,read(wrongSpawn).recipientPicked,read(wrongSpawn).throwUnknown], [2,2,64]);
  const mixed = evidenceRows();
  mixed[4].actor = stranger;
  assert.deepEqual([read(mixed).thrown,read(mixed).recipientPicked,read(mixed).pickupUnknown], [65,64,1]);
  const foreign = evidenceRows();
  Object.assign(foreign[9],{ actor: stranger, beforeStock: 5 });
  foreign[10].afterStock = 64;
  Object.assign(foreign[11],{ actor: stranger, afterStock: 7 });
  assert.deepEqual([read(foreign).recipientPicked,read(foreign).otherPicked], [64,2]);
  assert.deepEqual(read(foreign).collectors, [{uuid:owner,count:64},{uuid:stranger,count:2}]);
  foreign[11].afterStock = 6;
  assert.deepEqual([read(foreign).otherPicked,read(foreign).pickupUnknown], [0,2]);
  assert.deepEqual([read(evidenceRows().slice(0,8)).thrown,read(evidenceRows().slice(0,8)).pickupUnknown], [66,66]);
});

// 迟到收件仍可解释旧实体，同一角色随后另外丢出的同名物品不能计入旧工作。
test('late readback seals original throws while allowing later pickups', () => {
  const rows = evidenceRows();
  const next = { ...rows[0], item: id(9), networkId: 9, amount: 64, seq: 13 };
  rows.push(next,{ ...next, kind: 'item.added', seq: 14 });
  const open = handoffEvidence(rows.slice(0,8),mark,actor,owner,'dirt',66);
  const result = handoffEvidence(rows,mark,actor,owner,'dirt',66,open.throughSeq);
  assert.deepEqual([result.thrown,result.recipientPicked,result.throughSeq], [66,66,14]);
  assert.deepEqual(result.stacks,open.stacks);
  assert.throws(() => handoffEvidence(rows,mark,actor,owner,'dirt',66), /无法分开/);
});

// 投物与生成记录都缺运行号时，仅保留其他能独立核实的投物与收取。
test('missing entity identities retain unverified thrown units', () => {
  const missing = evidenceRows();
  delete missing[0].networkId;
  delete missing[1].networkId;
  const first = handoffEvidence(missing,mark,actor,owner,'dirt',66);
  assert.deepEqual([first.thrown,first.recipientPicked,first.throwUnknown],[2,2,64]);
});

// 合堆读回早于原事件时，该堆的后续收取归属仍待核实。
test('premature merge readback retains unverified pickup units', () => {
  const premature = evidenceRows();
  const merge = premature[6], readback = premature[7];
  premature[6] = { ...readback, seq:7, cause:8 };
  premature[7] = { ...merge, seq:8 };
  const second = handoffEvidence(premature,mark,actor,owner,'dirt',66);
  assert.deepEqual([second.thrown,second.recipientPicked,second.pickupUnknown],[66,64,2]);
});

// 原玩家最后收取时仍优先展示其身份，超过九名收取者的部分按实际件数结算。
test('bounded collector details preserve the recipient and omitted unit totals', () => {
  const rows = [];
  const add = (kind,fields) => rows.push({ kind:'item.'+kind, seq:rows.length+1, stream:mark.stream, tick:5, ...fields });
  for (let amount=1; amount<=11; ++amount) {
    const item = { item:id(100+amount), material:'minecraft:dirt', networkId:amount, amount };
    const collector = amount === 11 ? owner : id(20+amount);
    add('playerDrop',{ ...item,actor });
    add('added',item);
    add('pickup',{ ...item,actor:collector,remaining:0,beforeStock:0 });
    add('pickupReadback',{ cause:rows.length,actor:collector,afterStock:amount,ground:0 });
  }
  const proof = handoffEvidence(rows,mark,actor,owner,'dirt',66);
  assert.deepEqual([proof.recipientPicked,proof.otherPicked,proof.collectors.length,proof.omittedOtherUnits],[11,55,9,19]);
  assert.deepEqual(proof.collectors[0],{ uuid:owner,count:11 });
});
