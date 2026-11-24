const { test } = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const { trackSelfVitals } = require('../../plugins/mc/bridge/self-vitals.cjs');
const { observationInput } = require('../../plugins/mc/bridge/partner-observe.cjs');

// 使用可控的出生代次和协议包核对来源，保留公共氧气属性作为旁人污染的对照。
function fixture() {
  const bot = { _client: new EventEmitter(), entity: { id: 17 }, game: { dimension: 'overworld' },
    player: { uuid: '00112233-4455-6677-8899-aabbccddeeff' }, oxygenLevel: 20,
    registry: { entitiesByName: { player: { metadataKeys: ['flags','air_supply'] } } } };
  let world = 'a';
  const vitals = trackSelfVitals(bot,() => world);
  return { bot, vitals, read: () => vitals.read('inspect',world), spawn: () => { world = 'b'; },
    air: (entityId,value,key = 1) => bot._client.emit('entity_metadata',{ entityId, metadata: [{ key,value }] }),
    health: (health,food) => bot._client.emit('update_health',{ health,food }) };
}

// 旁人空气变化和无关元数据不刷新本人读数，查询时保留生命及空气各自的原采样。
test('self vitals only use packets for the current body and named air field', () => {
  const f = fixture();
  f.air(18,24);
  f.air(17,900,0);
  assert.deepEqual([f.read().health,f.read().air],[{ state:'unknown' },{ state:'unknown' }]);
  f.health(23.5,14);
  f.air(17,-8);
  const sample = f.read();
  assert.deepEqual([sample.actor,sample.entityId,sample.health.points,sample.health.food,sample.air.ticks],
    [f.bot.player.uuid,17,23.5,14,-8]);
  f.air(18,300);
  assert.deepEqual(f.read().air,sample.air);
  assert.deepEqual(f.read().health,sample.health);
  assert.equal(observationInput({ mode:'vitals' }).mode,'vitals');
  assert.throws(() => observationInput({ mode:'vitals', around:'player' }),{ code:'observe_vitals_parameters' });
});

// 角色替换、出生及维度变化分别隔离历史数据，坏包只撤下对应指标，关闭后释放监听。
test('vital readings stay with their original spawn, entity and dimension', () => {
  const f = fixture();
  f.air(17,280);
  f.health(12,10);
  f.bot.entity = { id:17 };
  assert.equal(f.read().air.state,'unknown');
  f.air(17,100);
  f.spawn();
  assert.equal(f.read().air.state,'unknown');
  assert.throws(() => f.vitals.read('old','a'),{ code:'observe_vitals_unavailable' });
  f.health(19,20);
  f.bot.game.dimension = 'the_nether';
  assert.equal(f.read().health.state,'unknown');
  f.health(12,15);
  f.air(17,50);
  f.health(11,21);
  f.air(17,0.5);
  assert.deepEqual([f.read().health.state,f.read().air.state],['unknown','unknown']);
  f.vitals.close();
  assert.deepEqual(f.bot._client.eventNames(),[]);
  assert.throws(f.read,{ code:'observe_vitals_unavailable' });
});
