const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { randomBytes, randomUUID } = require('node:crypto');
const { once } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const { withPaper } = require('./paper.cjs');
const { withPlayers, until } = require('./players.cjs');
const { observeWorld } = require('../../plugins/mc/bridge/world.cjs');
const { ownerInbox } = require('../../plugins/mc/bridge/chat.cjs');
const { inspectionService, landmarks } = require('../../plugins/mc/bridge/inspection.cjs');
const { openBridge } = require('../../plugins/mc/bridge/pipe.cjs');

// 同一独立协议玩家连续发送两批重复原话，按原话和投递号对照宿主 Trace 与玩家实收。
// 步骤：绑定真实游戏和本次数据目录 → 成对放行真实原话 → 核对显式发言、最终文本及玩家实收。
async function check(fixture, player, fish) {
  const [exe, pkg, data] = process.argv.slice(2).map(value => path.resolve(value));
  fs.mkdirSync(data, { recursive: true });
  const root = fs.mkdtempSync(path.join(data, 'paper-batch-'));
  const settings = path.join(root, 'user/plugins/mc.companion');
  fs.mkdirSync(settings, { recursive: true });
  const channel = randomUUID(), key = randomBytes(32).toString('hex');
  fs.writeFileSync(path.join(settings, 'settings.json'), JSON.stringify({ channel, bridgeKey: key }));
  const world = observeWorld(fish);
  const inbox = ownerInbox(fish, world, player.player.uuid, true, 32, undefined, true);
  const actions = inspectionService(fish, world, inbox, landmarks(), null, true, player.player.uuid);
  const batched = { ...inbox, read(generation, after) {
    const batch = inbox.read(generation, after);
    // 控制宿主读到两条原话的时点，内容和序号均取自玩家协议。
    if (batch && [2, 4].includes(batch.latest) && after === batch.latest - 1)
      return { ...batch, latest: after, messages: [], more: false };
    return batch;
  } };
  const bridge = await openBridge(channel, world, batched, key, actions);
  const heard = [];
  player.on('whisper', (sender, text) => { if (sender === fish.username) heard.push(text); });
  const child = spawn(exe, [pkg, root, 'partner-reply-batch'], { stdio: 'inherit', windowsHide: true });
  const closed = once(child, 'exit');
  try {
    for (const settled of [1, 3]) {
      await until(() => fs.existsSync(path.join(root, 'settled-' + settled)), '原回合结算');
      player.chat('听得到吗？');
      player.chat('听得到吗？');
    }
    assert.equal((await closed)[0], 0);
    await until(() => heard.length >= 4, '独立玩家收到四次私聊');
    assert.deepEqual(heard, Array(4).fill('我在这儿。'));
    const rows = fs.readFileSync(path.join(root, 'trace/events.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
    const receipts = rows.filter(r => r.kind === 'mail.received' && r.category === 'mc.partner.action' && r.body.skill === 'reply').map(r => r.body);
    assert.deepEqual(receipts.map(r => r.replyOrigin.sourceMessage), [2, 3, 4, 5]);
    assert.deepEqual(receipts.map(r => r.order), [3, 3, 5, 5]);
    assert.deepEqual(receipts.map(r => r.source === 'final_text'), [false, true, false, false]);
    assert.ok(receipts.every(r => r.result.delivery === 'sent' && r.result.text === heard[0]));
    assert.equal(new Set(receipts.map(r => r.bridgeWork)).size, 4);
    const repeats = rows.filter(r => r.kind === 'tool.exited' && r.callId === 'repeat-first');
    assert.deepEqual(repeats.map(r => JSON.parse(r.output).bridgeWork), [receipts[0].bridgeWork, receipts[2].bridgeWork]);
    fs.writeFileSync(path.join(root, 'player-receipts.json'), JSON.stringify({ player: player.player.uuid, heard, receipts }, null, 2));
    console.log(root);
  } finally {
    if (child.exitCode === null && !child.signalCode) { child.kill(); await closed; }
    await bridge.close();
  }
}
withPaper(fixture => withPlayers(fixture.port, (player, fish) => check(fixture, player, fish)))
  .catch(error => { console.error(error); process.exitCode = 1; });
