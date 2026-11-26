const assert = require('node:assert/strict');
const { writeFileSync } = require('node:fs');
const { join } = require('node:path');
const { until } = require('./players.cjs');

// 在隔离服以零删除上限查询伙伴库存，让独立协议玩家从服务端回消息读取数量。
// 步骤：监听当次私有标记 → 服务端清点并回传 → 与伙伴客户端已同步库存核对。
async function serverStock(fixture, player, sequence) {
  const prefix = 'dfy-stock-'+sequence+'=';
  let count;
  const received = text => { if (text.startsWith(prefix) && /^\d+$/.test(text.slice(prefix.length))) count = Number(text.slice(prefix.length)); };
  player.on('messagestr',received);
  try {
    fixture.send('execute store result score companion dfyStock run clear DfyCompanion minecraft:cobblestone 0');
    fixture.send('tellraw DfyGuestCheck [{"text":"'+prefix+'"},{"score":{"name":"companion","objective":"dfyStock"}}]');
    await until(() => Number.isInteger(count) && fixture.materialCount('cobblestone') === count,'独立玩家收到服务端伙伴库存数量');
    return count;
  } finally { player.off('messagestr',received); }
}

// 使用同一玩家原指箱现场验收取出，每次委托由独立玩家分别清点两个箱子及伙伴背包。
// 步骤：备好双箱和伙伴容量 → 从原指箱发起定量取出 → 核对双边实效 → 中断及重提回到原号。
async function checkChestWithdrawals(fixture, player, whispers, contents, actions, trace) {
  const rows = () => actions().filter(row => row.skill === 'withdraw_item' && row.result.withdrawal);
  const selected = actions().find(row => row.result.chest);
  const evidence = [], id = player.registry.itemsByName.cobblestone.id;
  let sequence = 0;
  fixture.send('scoreboard objectives add dfyStock dummy');
  // 独立玩家开两个箱，再读取服务端查询的伙伴库存及自己的背包。
  const sample = async () => {
    const companion = await serverStock(fixture,player,++sequence);
    const [left,right] = await contents();
    return {left,right,companion,player:player.inventory.count(id),sampledAtMs:Math.floor(performance.now())};
  };
  const cases = [
    {name:'normal',stock:12,taken:8},
    {name:'short',stock:3,taken:3,reason:'chest_item_short'},
    {name:'partial',stock:8,bag:61,taken:3,reason:'chest_inventory_partial'},
    {name:'full',stock:8,bag:64,taken:0,reason:'chest_inventory_full'},
    {name:'interrupted',stock:8,taken:8,reason:'chest_window_closed'}
  ];
  for (const [index,scenario] of cases.entries()) {
    const {stock,bag = 0,taken,name,reason} = scenario;
    fixture.send('data modify block 2 -60 -2 Items set value [{Slot:0b,id:"minecraft:cobblestone",count:'+stock+'}]');
    fixture.send('clear DfyCompanion');
    if (bag) {
      fixture.send('give DfyCompanion minecraft:stone 2304');
      fixture.send('item replace entity DfyCompanion hotbar.0 with minecraft:cobblestone '+bag);
    }
    const before = await sample();
    assert.deepEqual([before.left,before.right,before.companion,before.player],[stock,5,bag,0]);
    await player.lookAt(player.entity.position.offset(0,6,0),true);
    await until(() => fixture.ownerEntity()?.pitch > 1,'玩家从原指箱转向上方');
    const interrupted = name === 'interrupted', start = whispers.length;
    if (interrupted) fixture.interruptChest();
    player.chat(index % 2 ? '请把左边的箱子里的8块圆石取到伙伴背包里' : '从左箱取出八块圆石，放进你的背包');
    const row = await until(() => rows()[index],'取出原工作实际退出',20000);
    const proof = row.result.withdrawal;
    assert.deepEqual([proof.direction,proof.destination,proof.work,proof.observation,proof.pointedMessage,proof.sourceMessage,proof.cell],
      ['withdraw','companion_inventory',row.work,selected.work,selected.order,row.order,{x:2,y:-60,z:-2}]);
    assert.deepEqual([proof.before.container.stored,proof.before.stock.stored,proof.state,
      proof.withdrawn,proof.remaining,row.result.reasonCode,proof.attempted],
      [stock,bag,interrupted ? 'pending' : 'verified',interrupted ? undefined : taken,interrupted ? undefined : 8-taken,reason,taken > 0]);
    const report = '从左箱取到我的背包 '+taken+' 块，剩余 '+(8-taken)+' 块。';
    await until(() => whispers.slice(start).includes(interrupted ? '取出工作 '+row.work+' 的实取量待核实。' : report),'只按已核实数量回答玩家');
    const after = await sample();
    assert.deepEqual([after.left,after.right,after.companion,after.player],[stock-taken,5,bag+taken,0]);
    assert.equal((await fixture.repeatChest(row,row.work)).action.exited,true);
    const duplicateId = 'repeat-withdraw-'+index;
    await fixture.repeatChest(row,duplicateId);
    const duplicate = await until(() => actions().find(entry => entry.work === duplicateId),'换号重提返还原取出工作');
    assert.deepEqual([duplicate.result.originalWork,duplicate.result.withdrawal],[row.work,undefined]);
    let recheck;
    if (interrupted) {
      const offset = whispers.length;
      player.chat('复查取出工作 '+row.work);
      await until(() => whispers.slice(offset).includes(report),'原工作重开同一箱查回取出数');
      const read = actions().find(entry => entry.result.chestReceipt?.work === row.work);
      const settled = await until(() => trace().find(entry => entry.kind === 'binding.work_settled' && entry.work === row.work &&
        entry.revision === 4 && entry.finalEffect && entry.body.withdrawn === taken),'原工作收到迟到双边实效');
      assert.deepEqual([settled.body.destination,read.result.chestReceipt.remaining],['companion_inventory',8-taken]);
      recheck = {query:read.work,work:settled.work,revision:settled.revision,withdrawn:taken,remaining:8-taken};
    }
    const final = await sample();
    assert.deepEqual([rows().length,final.left,final.right,final.companion,final.player],[index+1,after.left,after.right,after.companion,after.player]);
    evidence.push({scenario:name,generation:proof.generation,dimension:proof.dimension,selected:proof.cell,other:{x:2,y:-60,z:2},
      observation:proof.observation,pointedMessage:proof.pointedMessage,sourceMessage:proof.sourceMessage,work:row.work,
      direction:proof.direction,destination:proof.destination,status:proof.state,withdrawn:proof.withdrawn,remaining:proof.remaining,
      reason,attempted:proof.attempted,rejectedDuplicate:duplicate.result.originalWork,before,after,final,recheck});
    writeFileSync(join(fixture.evidence,'withdraw-evidence.json'),JSON.stringify(evidence,null,2));
  }
}

module.exports = { checkChestWithdrawals };
