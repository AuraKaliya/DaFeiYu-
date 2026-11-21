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
  const inbox = ownerInbox(bot, { read: () => ({ generation, dimension: 'overworld' }) }, player.uuid, true, 8, undefined, true);
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
  const [message, departure] = inbox.read(generation, 2).messages;
  assert.equal(message.sequence, 3);
  assert.equal(message.text, '停，我来');
  assert.equal(message.type, 'player.message');
  assert.equal(message.sceneAtMessage.position, null);
  assert.deepEqual(departure, { type: 'player.left', sequence: 4 });
  generation = 'second';
  assert.deepEqual(inbox.read(generation, 0).messages, []);
  bot.players.owner = player;
  bot.emit('playerJoined', player);
  assert.deepEqual(inbox.read(generation, 0).presence, { type: 'player.entered', sequence: 1 });
  assert.equal(inbox.get('first', 1), null);
  inbox.close();
  for (const event of ['playerJoined', 'playerLeft']) assert.equal(bot.listenerCount(event), 0);
});

// 在两处缺口之间移动后，旧聊天仍携带发送时的坐标。
test('伙伴原话保留采样时的玩家位置', () => {
  const bot = new EventEmitter();
  bot._client = new EventEmitter();
  const uuid = '34fbcf7f-a9c8-3945-98b4-00e02e34ab18';
  const position = { x: -0.5, y: -57, z: 9.25 };
  const entity = { position, headYaw: 1.5, pitch: -0.25 };
  bot.players = { owner: { uuid, entity } };
  const inbox = ownerInbox(bot, { read: () => ({ generation: 'world', dimension: 'overworld' }) },
    uuid, true, 8, undefined, true);
  bot._client.emit('playerChat', { sender: uuid, plainMessage: '补我旁边这个缺口' });
  position.x = 7;
  entity.headYaw = 0;
  const message = inbox.read('world', 0).messages[0];
  assert.deepEqual(message.sceneAtMessage.position, { x: -0.5, y: -57, z: 9.25 });
  assert.equal(message.sceneAtMessage.dimension, 'overworld');
  assert.deepEqual(message.sceneAtMessage.facing, { yaw: 1.5, pitch: -0.25 });
  assert.ok(Number.isSafeInteger(message.sceneAtMessage.sampledAtMs));
  assert.deepEqual(inbox.get('world', message.sequence), message);
  inbox.close();
});
