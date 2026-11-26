const assert = require('node:assert/strict');

// 从原 Trace 取场景和配对工具结果，只在末条消息的角色、同下标来源和状态版本吻合时分出尾注。
// 步骤：核对宿主来源 → 保留原消息顺序与工具调用号 → 普通决策单列已提供的只读工具。
function scenarioInput(row) {
  const { messages, tools } = row.input;
  const index = messages.length - 1, last = messages[index];
  if (index < 1 || last.role !== 'user' || row.messageEvidence?.length !== messages.length ||
      row.messageEvidence[index]?.source !== 'host_status') return row.input;
  let status;
  try { status = JSON.parse(last.content); } catch { return row.input; }
  if (status?.schema !== 'dafeiyu.agent_status/1') return row.input;

  const original = messages.slice(0,index);
  const source = row.messageEvidence[original.findLastIndex(message => message.role === 'user')]?.source;
  const direct = row.projection?.purpose === 'direct' && source && source !== 'host_plan' && source !== 'host_control';
  return { ...row.input, messages:original, tools:direct ? tools.filter(tool => tool.name !== 'status_read') : tools, status };
}

// 将试点状态逐项对回同会话先前接纳的任务修订，集成模式另核对宿主已剥离传输字段。
// 步骤：沿工具回执保存原目标 → 检查初始基线缺样 → 按原主体和修订核对后续送模样本。
function checkTaskGoalTrace(rows, definition, player, requireProjection) {
  const goals = new Map();
  let initial = false, sampled = false;
  for (const row of rows) {
    if (row.kind === 'tool.exited' && row.toolId === 'mc.companion/task_update' && row.succeeded) {
      const result = JSON.parse(row.output);
      if (result.status === 'updated') goals.set(row.session+'/'+result.task.revision,result.task.goal);
    }
    if (row.kind !== 'model.prepared' || row.source !== 'plugin' || row.step !== 1) continue;
    const { messages, status } = scenarioInput(row);
    const scene = JSON.parse(messages.at(-1).content);
    if (!scene.factFrame) continue;
    if (requireProjection) assert.ok(status,'选定玩家的正式请求缺少宿主状态尾注');
    if (status) assert.equal(Object.hasOwn(scene,'decisionStatusSamples'),false,'领域消息混入了状态传输字段');
    const samples = (status ? status.items : scene.decisionStatusSamples ?? []).filter(item => item.key === definition.key);
    if (scene.factFrame.mode === 'baseline' && scene.task?.revision === 0) {
      assert.deepEqual(samples,[]);
      initial = true;
    }
    for (const sample of samples) {
      const original = goals.get(row.session+'/'+sample.fact);
      assert.ok(original,'原会话缺少该事实号对应的已接纳任务');
      assert.deepEqual([sample.subject,sample.version,sample.value],[player,definition.version,{goal:original}]);
      sampled = true;
    }
  }
  assert.ok(initial && sampled,'需要同时核对无目标基线及实际任务修订');
}

// 核对实际送模中本体人格、领域指引、原话和工具各自的位置。
// 步骤：核对入口固定约定 → 从事实正文核验插件指引及适用状态 → 返回当轮消息与工具。
function readPartnerContext(row) {
  const { messages, tools } = scenarioInput(row);
  const [system, message] = messages;
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
  return { input, system: system.content, guidance, tools };
}

module.exports = { checkTaskGoalTrace, readPartnerContext, scenarioInput };
