const { test } = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const { trackSelfVitals } = require('../../plugins/mc/bridge/self-vitals.cjs');
const { observeShore } = require('../../plugins/mc/bridge/shore.cjs');
const { Position } = require('./test-position.cjs');
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

// 让同一本人的余气包分别遇到干岸、封闭水体和缺失地形，核对纯观察的证据边界。
test('shore evidence separates a dry route, known obstruction and incomplete coverage', () => {
  const f = fixture();
  f.bot.entity.position = new Position(-0.01,-60.000041,0.5);
  const origin = f.bot.entity.position.floored();
  const solid = { name:'stone',boundingBox:'block',shapes:[[0,0,0,1,1,1]] };
  const water = {name:'water',boundingBox:'empty'}, air = {name:'air',boundingBox:'empty'};
  f.bot.blockAt = p => p.equals(origin) ? water : solid;
  const probe = () => assert.fail('sealed or unsampled water requested a route');
  assert.equal(observeShore(f.bot,f.read(),probe).reason,'air_unsampled');
  f.air(17,12);
  const sample = f.read();
  let shore = observeShore(f.bot,sample,probe);
  assert.deepEqual([shore.status,shore.reason,shore.coverage.origin.y,shore.airSampledAtMs],['blocked','sealed_volume',-61,sample.air.sampledAtMs]);
  f.bot.blockAt = p => p.equals(origin) ? water : p.y <= origin.y ? solid : air;
  const route = (from,stands,inside) => {
    assert.equal(from,f.bot.entity.position);
    const target = stands.find(p => p.equals(origin.offset(1,1,0)));
    assert.ok(target && inside(target));
    return {status:'success',path:[target]};
  };
  shore = observeShore(f.bot,sample,route);
  assert.deepEqual([shore.status,shore.stand],['reachable',{x:0,y:-60,z:0}]);
  assert.equal(observeShore(f.bot,{...sample,air:{...sample.air,waterCell:origin.offset(9,0,0)}},probe).reason,'air_location_unconfirmed');
  assert.equal(observeShore(f.bot,sample,() => ({status:'noPath'})).reason,'no_path_in_range');
  assert.equal(observeShore(f.bot,sample,() => ({status:'partial'})).status,'unconfirmed');
  const terrain = f.bot.blockAt;
  f.bot.blockAt = p => p.equals(origin.offset(0,1,1)) ? null : terrain(p);
  assert.equal(observeShore(f.bot,sample,() => ({status:'noPath'})).reason,'terrain_unknown');
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

// 反复耗气和短时正值沿用原记录，只有持续回升才结算，再次耗尽另起一份。
test('air depletion coalesces real packets and confirms recovery before starting another episode', () => {
  const f = fixture();
  f.air(17,4);
  assert.equal(f.read().airEpisode,undefined);
  for (const ticks of [0,-1,-4,1,0]) f.air(17,ticks);
  const original = f.read().airEpisode;
  assert.deepEqual([original.episode,original.phase,original.samples,original.minimumTicks,original.lastTicks],[1,'depleted',5,-4,0]);
  const notice = f.vitals.notice('a',0);
  assert.deepEqual([notice.sequence,notice.airEpisode.phase,notice.airEpisode.samples],[1,'depleted',1]);
  assert.deepEqual(f.vitals.notice('a',0),notice);
  assert.equal(f.vitals.notice('a',1),undefined);
  f.air(18,300);
  assert.deepEqual(f.read().airEpisode,original);
  for (const ticks of [3,3,2]) f.air(17,ticks);
  assert.deepEqual([f.read().airEpisode.phase,f.read().airEpisode.recoveryTicks],['recovering',2]);
  f.air(17,6);
  const recovered = f.read().airEpisode;
  f.air(17,300);
  assert.deepEqual([recovered.episode,recovered.phase,recovered.samples,recovered.minimumTicks,recovered.lastTicks],[1,'recovered',9,-4,6]);
  assert.deepEqual(f.read().airEpisode,recovered);
  assert.deepEqual(f.vitals.notice('a',1).airEpisode,recovered);
  assert.equal(f.vitals.notice('a',2),undefined);
  f.air(17,-1);
  assert.deepEqual([f.read().airEpisode.episode,f.read().airEpisode.samples],[2,1]);
  f.spawn();
  assert.equal(f.read().airEpisode,undefined);
  assert.equal(f.vitals.notice('b',0),undefined);
});
