const assert = require('node:assert/strict');
const { writeFileSync } = require('node:fs');
const { join } = require('node:path');
const { until } = require('./players.cjs');
const { serverStock } = require('./chest-withdrawal-check.cjs');

// 用同一玩家委托串起原取出与原给物两个工作，独立读取双箱、伙伴背包及接收玩家库存。
// 步骤：准备缺货或满包现场 → 核对两笔工作的物品流向 → 重提及复查 → 核实迟到拾取。
async function checkChestDelivery(fixture, player, whispers, contents, actions, trace) {
  const evidence = [], item = 'cobblestone', id = player.registry.itemsByName[item].id;
  const selection = actions().find(row => row.result.chest);
  const withdrawals = () => actions().filter(row => row.result.withdrawal);
  const gifts = () => actions().filter(row => row.skill === 'give_item' && row.result.transfer?.attemptedClicks);
  let sequence = 0;
  fixture.send('scoreboard objectives add dfyStock dummy');
  // 由独立玩家分别读两箱、自身槽位及服务端对伙伴的计数。
  const sample = async () => {
    const companion = await serverStock(fixture,player,++sequence);
    const [left,right] = await contents();
    return {left,right,companion,recipient:player.inventory.count(id)};
  };
  // 每次完整等待本条复查的实际新回复，避免复用同文历史回复。
  const ask = async (line,expected) => {
    const start = whispers.length;
    player.chat(line);
    await until(() => whispers.slice(start).includes(expected),'收到本条工作复查的实际回复',20000);
  };
  for (const [index,scenario] of [{name:'short',stock:3,bag:11},{name:'empty',stock:0,bag:11},{name:'late',stock:8,bag:0}].entries()) {
    const {name,stock,bag} = scenario, late = name === 'late';
    fixture.send('data modify block 2 -60 -2 Items set value '+(stock ? '[{Slot:0b,id:"minecraft:cobblestone",count:'+stock+'}]' : '[]'));
    fixture.send('clear DfyCompanion');
    if (bag) fixture.send('give DfyCompanion minecraft:cobblestone '+bag);
    fixture.send('clear DfyGuestCheck');
    if (late) fixture.send('give DfyGuestCheck minecraft:stone 2304');
    const before = await sample();
    assert.deepEqual(before,{left:stock,right:5,companion:bag,recipient:0});
    await until(() => !late || player.inventory.emptySlotCount() === 0,'接收玩家本局容量已同步');
    await player.lookAt(player.entity.position.offset(0,6,0),true);
    await until(() => fixture.ownerEntity()?.pitch > 1,'委托时玩家已转离原箱');
    const giftCount = gifts().length, start = whispers.length;
    player.chat(index % 2 ? '请把左边的箱子里的8块圆石取出来给我' : '从左箱取出八块圆石，交给我');
    const take = await until(() => withdrawals()[index],'同一委托下的取出工作',20000);
    const proof = take.result.withdrawal;
    assert.deepEqual([proof.observation,proof.work,proof.sourceMessage,proof.deliverTo,proof.state,proof.withdrawn,proof.remaining,
      proof.after.container.stored,proof.after.stock.stored],[selection.work,take.work,take.order,player.player.uuid,'verified',stock,8-stock,0,bag+stock]);
    const give = stock ? await until(() => gifts()[giftCount],'按实取数量交付的原工作',20000) : undefined;
    const taken = '从左箱取到我的背包 '+stock+' 块，剩余 '+(8-stock)+' 块。';
    const report = picked => '左箱实取 '+stock+' 块，已投 '+stock+' 块，你已收 '+picked+' 块。';
    await until(() => whispers.slice(start).includes(stock ? report(late ? 0 : stock) : taken),'按实取、实投和实收向委托玩家报告');
    if (give) {
      const transfer = give.result.transfer, receipt = give.result.receipt;
      assert.deepEqual([give.order,give.arguments.withdrawalWork,transfer.withdrawalWork,transfer.recipient,transfer.requested,transfer.submitted,
        transfer.stockBefore,transfer.stockAfter,receipt.thrown,receipt.recipientPicked,receipt.pickupUnknown],
        [take.order,take.work,take.work,player.player.uuid,stock,stock,bag+stock,bag,stock,late ? 0 : stock,late ? stock : 0]);
    }
    const delivered = await sample();
    assert.deepEqual(delivered,{left:0,right:5,companion:bag,recipient:late ? 0 : stock});
    const repeatTake = 'repeat-take-'+index;
    await fixture.repeatChest(take,repeatTake);
    const rejectedTake = await until(() => actions().find(row => row.work === repeatTake),'重提取出回指原号');
    assert.equal(rejectedTake.result.originalWork,take.work);
    // 换一份真实备料观察，核对原取出额度和原给物认领都不能绕过。
    const offerId = 'new-offer-'+index;
    await fixture.repeatChest(take,offerId,'observe',{mode:'handoff',recipient:player.player.uuid,item,count:stock || 8});
    const offer = await until(() => actions().find(row => row.work === offerId),'重新读取玩家和伙伴背包');
    assert.equal(offer.result.handoff.stock.available,bag);
    const giveArgs = {observation:offerId,recipient:player.player.uuid,item,count:stock || 8,withdrawalWork:take.work};
    const duplicateId = 'repeat-chest-gift-'+index;
    await fixture.repeatChest(take,duplicateId,'give_item',giveArgs);
    const rejectedGive = await until(() => actions().find(row => row.work === duplicateId),'换备料观察后核对首次给物认领');
    assert.equal(rejectedGive.result.reasonCode,give ? 'handoff_already_started' : 'chest_delivery_unconfirmed');
    if (give) assert.equal(rejectedGive.result.originalWork,give.work);
    else {
      const {withdrawalWork,...unlinked} = giveArgs;
      await fixture.repeatChest(take,'missing-withdrawal','give_item',unlinked);
      const denied = await until(() => actions().find(row => row.work === 'missing-withdrawal'),'拒绝省略原取出号动用原库存');
      assert.equal(denied.result.reasonCode,'chest_delivery_source_required');
    }
    await ask('复查取出工作 '+take.work,taken);
    let revision, checks = [];
    if (give) {
      await ask('复查交物工作 '+give.work,report(late ? 0 : stock));
      if (late) {
        fixture.send('clear DfyGuestCheck minecraft:stone');
        await until(() => player.inventory.count(id) === stock,'腾包后目标玩家独立读到八块圆石');
      }
      await ask('复查交物工作 '+give.work,report(stock));
      checks = actions().filter(row => row.result.handoffReceipt?.work === give.work);
      assert.ok(checks.length >= 2 && checks.every(row => row.result.handoffReceipt.withdrawalWork === take.work));
      if (late) {
        const settled = await until(() => trace().find(row => row.kind === 'binding.work_settled' && row.work === give.work &&
          row.revision === 4 && row.finalEffect && row.body.withdrawalWork === take.work && row.body.recipientPicked === stock),'迟收仍连接两笔原工作');
        revision = settled.revision;
      }
    }
    const final = await sample();
    assert.deepEqual(final,{left:0,right:5,companion:bag,recipient:stock});
    assert.deepEqual([withdrawals().length,gifts().length],[index+1,giftCount+(stock ? 1 : 0)]);
    const frames = trace().filter(row => row.kind === 'model.prepared').map(row => row.input);
    assert.ok(frames.some(frame => frame.tools.some(tool => tool.name.endsWith('_give_item') &&
      JSON.parse(tool.parameters).properties.withdrawalWork) && frame.tools.some(tool => tool.name.endsWith('_withdraw_item'))));
    const calls = frames.flatMap(frame => frame.messages.flatMap(message => message.toolCalls ?? []));
    assert.ok(calls.some(call => call.name.endsWith('_withdraw_item') && JSON.parse(call.arguments).sourceMessage === take.order));
    assert.equal(calls.some(call => call.name.endsWith('_give_item') && JSON.parse(call.arguments).withdrawalWork === take.work &&
      JSON.parse(call.arguments).count === stock),Boolean(give));
    assert.ok(!trace().some(row => row.body?.factReceiptError));
    evidence.push({scenario:name,owner:player.player.uuid,generation:proof.generation,cell:proof.cell,other:{x:2,y:-60,z:2},
      selection:selection.work,sourceMessage:take.order,withdrawal:take.work,give:give?.work,requested:8,withdrawn:stock,remaining:8-stock,
      thrown:give ? give.result.receipt.thrown : 0,initiallyPicked:delivered.recipient,checks:checks.map(row => row.work),revision,
      rejectedTake:rejectedTake.result.originalWork,rejectedGive:rejectedGive.result.originalWork ?? rejectedGive.result.reasonCode,before,delivered,final});
    writeFileSync(join(fixture.evidence,'chest-delivery-evidence.json'),JSON.stringify(evidence,null,2));
  }
}

module.exports = { checkChestDelivery };
