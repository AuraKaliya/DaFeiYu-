const assert = require('node:assert/strict');

// 核对实际送模中本体人格、领域指引、原话和工具各自的位置。
// 步骤：核对入口固定约定 → 从事实正文核验插件指引及适用状态 → 返回当轮消息与工具。
function readPartnerContext(row) {
  const [system, message] = row.input.messages;
  const header = '\nMinecraft 同场规则\n';
  const start = system.content.indexOf(header);
  assert.equal(system.role, 'system');
  assert.equal(message.role, 'user');
  assert.equal(row.personaProfile, 'dafeiyu/default/2');
  assert.match(row.promptTemplate, /^sha256:/);
  assert.ok(system.content.startsWith('【职责与信息来源 v1】') && start > 0);
  assert.match(system.content, /【人格与风格 v1】\n你叫大肥鱼。/);
  const end = system.content.indexOf('\n【授权与行动边界 v1】', start);
  assert.ok(end > start + header.length);
  const fixed = JSON.parse(system.content.slice(start + header.length, end));
  assert.deepEqual(Object.keys(fixed), ['flowGuidance']);
  assert.deepEqual(Object.keys(fixed.flowGuidance), ['scene']);
  assert.match(fixed.flowGuidance.scene, /mcGuidance/);
  const input = JSON.parse(message.content);
  const guidance = input.mcGuidance ?? {};
  const flow = guidance.flowGuidance ?? {};
  assert.equal(flow.scene, undefined);
  for (const [purpose, entries] of Object.entries(guidance)) {
    assert.ok(['flowGuidance', 'domainExperience', 'outputConstraints'].includes(purpose));
    assert.ok(entries && typeof entries === 'object' && !Array.isArray(entries));
    assert.ok(Object.values(entries).every(value => typeof value === 'string' && value.length));
  }
  for (const field of ['taskHandoff', 'pendingQuestion', 'questionResult', 'planRejection']) {
    const plural = field === 'pendingQuestion' || field === 'questionResult';
    assert.equal(Object.hasOwn(flow, field === 'taskHandoff' ? 'handoff' : field), Object.hasOwn(input,field) ||
      (plural && Object.hasOwn(input,field+'s')));
  }
  assert.doesNotMatch(JSON.stringify(guidance), /wallSurvey|route\.id|afterPickup|placementOptions/);
  return { input, system: system.content, guidance, tools: row.input.tools };
}

module.exports = { readPartnerContext };
