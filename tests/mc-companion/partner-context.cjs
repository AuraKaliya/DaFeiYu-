const assert = require('node:assert/strict');

// 核对实际送模中本体人格、领域指引、原话和工具各自的位置。
// 步骤：读取系统段和事实正文 → 校验结构和适用状态 → 返回本轮工具契约。
function readPartnerContext(row) {
  const [system, message] = row.input.messages;
  const header = '\nMinecraft 同场规则\n';
  const start = system.content.indexOf(header);
  assert.equal(system.role, 'system');
  assert.equal(message.role, 'user');
  assert.equal(row.personaProfile, 'dafeiyu/default/1');
  assert.match(row.promptTemplate, /^sha256:/);
  assert.ok(system.content.startsWith('你叫大肥鱼。') && start > 0);
  const guidance = JSON.parse(system.content.slice(start + header.length));
  const input = JSON.parse(message.content);
  const flow = guidance.flowGuidance;
  assert.equal(typeof flow.scene, 'string');
  for (const [purpose, entries] of Object.entries(guidance)) {
    assert.ok(['flowGuidance', 'domainExperience', 'outputConstraints'].includes(purpose));
    assert.ok(entries && typeof entries === 'object' && !Array.isArray(entries));
    assert.ok(Object.values(entries).every(value => typeof value === 'string' && value.length));
  }
  for (const field of ['taskHandoff', 'pendingQuestion', 'questionResult', 'planRejection']) {
    assert.equal(Object.hasOwn(flow, field === 'taskHandoff' ? 'handoff' : field), Object.hasOwn(input, field));
  }
  assert.doesNotMatch(JSON.stringify(guidance), /wallSurvey|route\.id|afterPickup|placementOptions/);
  return { input, guidance, tools: row.input.tools };
}

module.exports = { readPartnerContext };
