const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const { ownerInbox } = require('../../plugins/mc/bridge/chat.cjs');

// 用真实收件器核对名单快照、身份过滤、重复通知、重入世界及监听释放。
test('伙伴名单变化和绑定玩家原话按代次只记录一次', () => {
  const bot = new EventEmitter();
  bot._client = new EventEmitter();
  const player = { uuid: '34fbcf7f-a9c8-3945-98b4-00e02e34ab18' };
  bot.players = { owner: player };
  let generation = 'first';
  const inbox = ownerInbox(bot, { read: () => ({ generation }) }, player.uuid, true, 8, undefined, true);
  const chat = (sender, text) => bot._client.emit('playerChat', { sender, plainMessage: text });
  chat(player.uuid, '启用之前的原话');
  const initial = inbox.read(generation, 0);
  assert.deepEqual(initial.presence, { type: 'player.entered', sequence: 2 });
  assert.deepEqual(inbox.read(generation, initial.latest).messages, []);
  bot.emit('playerJoined', player);
  chat('other', '停，我来');
  chat(player.uuid, '停，我来');
  delete bot.players.owner;
  bot.emit('playerLeft', player);
  bot.emit('playerLeft', player);
  assert.deepEqual(inbox.read(generation, 2).messages, [
    { type: 'player.message', sequence: 3, text: '停，我来' }, { type: 'player.left', sequence: 4 },
  ]);
  generation = 'second';
  assert.deepEqual(inbox.read(generation, 0).messages, []);
  bot.players.owner = player;
  bot.emit('playerJoined', player);
  assert.deepEqual(inbox.read(generation, 0).presence, { type: 'player.entered', sequence: 1 });
  assert.equal(inbox.get('first', 1), null);
  inbox.close();
  for (const event of ['playerJoined', 'playerLeft']) assert.equal(bot.listenerCount(event), 0);
});
