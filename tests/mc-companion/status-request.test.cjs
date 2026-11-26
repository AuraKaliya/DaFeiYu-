const assert = require('node:assert/strict');
const test = require('node:test');
const { checkTaskGoalTrace, scenarioInput } = require('./partner-context.cjs');

// 回放同一正式请求的两种格式，核对原话、工具调用号、宿主来源和原始记录。
test('领域消息和配对工具结果在新旧正式请求中保持原身份', () => {
  const scene = { role:'user', content:'{"player":"paired","trigger":"game.event"}' };
  const result = { role:'tool', toolCallId:'read-7', content:'{"work":"1.4"}' };
  const status = { schema:'dafeiyu.agent_status/1', snapshot:'S1/2', items:[] };
  const tail = { role:'user', content:JSON.stringify(status) };
  const read = { name:'status_read' }, observe = { name:'p_fixture_observe' };
  for (const messages of [[scene],[scene,{role:'assistant',toolCalls:[{id:'read-7'}]},result]]) {
    const row = { input:{messages,tools:[observe]}, projection:{purpose:'direct'} };
    assert.strictEqual(scenarioInput(row).messages.at(-1),messages.at(-1));
    const oldSources = messages.map(message => ({source:message.role === 'user' ? 'plugin' : message.role}));
    const current = { ...row,input:{messages:[...messages,tail],tools:[read,observe]},
      messageEvidence:[...oldSources,{source:'host_status'}] };
    const chosen = scenarioInput(current);
    assert.strictEqual(chosen.messages.at(-1),messages.at(-1));
    assert.deepEqual(chosen.tools,[observe]);
    assert.deepEqual(chosen.status,status);
    assert.strictEqual(current.input.messages.at(-1),tail);
    assert.deepEqual(current.input.tools,[read,observe]);
    assert.deepEqual(scenarioInput({...current,projection:{purpose:'plan_generation'}}).tools,[read,observe]);
    for (const evidence of [undefined,[{source:'host_status'}],[...oldSources,{source:'human'}]])
      assert.strictEqual(scenarioInput({...current,messageEvidence:evidence}).messages.at(-1),tail);
    const other = {role:'user',content:'{"schema":"another/1"}'};
    assert.strictEqual(scenarioInput({...current,input:{...current.input,messages:[...messages,other]}}).messages.at(-1),other);
  }
});

// 回放选定玩家的缺样基线与首次目标，核对现行投影、旧补丁和被替换的原事实号。
test('任务目标从同会话原工具回执追到决策状态', () => {
  const player = 'paired', definition = {key:'mc.companion.task_goal',version:1};
  const sample = {...definition,subject:player,fact:3,value:{goal:'清理门边'}};
  const accepted = {kind:'tool.exited',session:7,toolId:'mc.companion/task_update',succeeded:true,
    output:JSON.stringify({status:'updated',task:{revision:3,goal:'清理门边'}})};
  // 分别构造真实格式的领域正文与带同下标来源的宿主尾注。
  const frame = (baseline, projected, samples) => {
    const scene = {player,factFrame:{mode:baseline ? 'baseline' : 'delta'},task:{revision:baseline ? 0 : 3}};
    if (!projected && samples.length) scene.decisionStatusSamples = samples;
    const row = {kind:'model.prepared',source:'plugin',session:7,step:1,
      input:{messages:[{role:'system',content:'rules'},{role:'user',content:JSON.stringify(scene)}],tools:[]}};
    if (projected) {
      row.input.messages.push({role:'user',content:JSON.stringify({schema:'dafeiyu.agent_status/1',items:samples})});
      row.messageEvidence = [{source:'rules'},{source:'plugin'},{source:'host_status'}];
    }
    return row;
  };
  for (const projected of [false,true]) {
    const baseline = frame(true,projected,[]), changed = frame(false,projected,[sample]);
    checkTaskGoalTrace([baseline,accepted,changed],definition,player,projected);
    assert.throws(() => checkTaskGoalTrace([baseline,{...accepted,session:8},changed],definition,player,projected));
    assert.throws(() => checkTaskGoalTrace([baseline,accepted,frame(false,projected,[{...sample,fact:4}])],definition,player,projected));
  }
});
