import { decodedObject, id, object, objects, text } from './trace-file.ts';
import type { Data } from './trace-file.ts';
import { mcInput } from './rounds.ts';
import type { RecordEntry, Round } from './rounds.ts';

export interface ToolCall {
  readonly key: string;
  readonly name: string;
  readonly callId?: string;
  readonly records: readonly RecordEntry[];
  readonly admitted?: RecordEntry;
  readonly exited?: RecordEntry;
}

export interface ModelStage {
  readonly key: string;
  readonly step: string;
  readonly attempt: string;
  readonly records: readonly RecordEntry[];
  readonly prepared?: RecordEntry;
  readonly submitted?: RecordEntry;
  readonly exited?: RecordEntry;
  readonly rewritten?: RecordEntry;
  readonly tools: readonly ToolCall[];
}

// 构造页内唯一的送模步骤身份，保留不同尝试与不同原始调用。
export function stageKey(entry: RecordEntry): string | undefined {
  if (entry.step && entry.attempt && (entry.kind.startsWith('model.') || entry.kind.startsWith('tool.')))
    return `${entry.step}/${entry.attempt}`;
}

// 将同批候选、接纳和退出按宿主候选号合并，缺少身份的项仍可查原行。
function toolCalls(entries: readonly RecordEntry[]): ToolCall[] {
  const calls = new Map<string, RecordEntry[]>();
  for (const entry of entries) {
    if (!entry.kind.startsWith('tool.')) continue;
    const key = entry.candidate ?? (entry.callId ? `call:${entry.callId}` : `line:${entry.ref.line}`);
    const records = calls.get(key) ?? [];
    if (!records.length) calls.set(key, records);
    records.push(entry);
  }
  return Array.from(calls, ([key, records]) => {
    const admitted = records.find(entry => entry.kind === 'tool.admitted');
    const exited = records.find(entry => entry.kind === 'tool.exited');
    return { key, records, admitted, exited, name: exited?.name ?? admitted?.name ?? records[0].name ?? '未识别的工具',
      callId: records[0].callId };
  });
}

// 按实际送模步和尝试组装模型阶段及随附工具。
// 步骤：按原身份聚合 → 保留可选阶段节点 → 关联同批工具。
export function modelStages(round: Round): ModelStage[] {
  const grouped = new Map<string, RecordEntry[]>();
  for (const row of round.events) {
    const key = stageKey(row);
    if (!key) continue;
    const records = grouped.get(key) ?? [];
    if (!records.length) grouped.set(key, records);
    records.push(row);
  }
  return Array.from(grouped, ([key, records]) => {
    const [step, attempt] = key.split('/');
    return { key, step, attempt, records,
      prepared: records.find(entry => entry.kind === 'model.prepared'),
      submitted: records.find(entry => entry.kind === 'model.submitted'),
      exited: records.find(entry => entry.kind === 'model.exited'),
      rewritten: records.find(entry => entry.kind === 'model.rewritten'),
      tools: toolCalls(records) };
  });
}

// 只计算同一宿主时钟的已知区间，零值代表实际可测。
export function milliseconds(start?: string, end?: string): number | undefined {
  if (!start || !end) return undefined;
  const delta = BigInt(end) - BigInt(start);
  return delta < 0n ? undefined : Number(delta) / 1e6;
}

// 分段计算模型排队、实际调用和宿主观察退出的时间。
export function stageTiming(collected: ModelStage) {
  const timing = collected.exited?.timing;
  const submitted = timing?.submitted ?? collected.submitted?.timing?.submitted;
  const started = timing?.started ?? collected.submitted?.timing?.started;
  return {
    queue: milliseconds(submitted, started),
    service: milliseconds(started, timing?.finished),
    readback: milliseconds(timing?.finished, collected.exited?.time),
  };
}

export interface SystemParts {
  readonly title: string;
  readonly persona: string;
  readonly guidance: Data;
}

const guidanceHeading = /^Minecraft (?:同场规则|动作复核|委托规则)\r?$/m;
const ruleHeadings = ['【职责与信息来源 v1】', '【授权与行动边界 v1】', '【判断与结算 v1】', '【输出与投递 v1】', '【人格与风格 v1】'];

// 对可证实的 MC 系统段拆出人格和领域指引，原文仍可直接查看。
// 步骤：核对新版五个独占行或保留旧布局 → 只从原领域段读 JSON → 对应同版人格正文。
export function systemParts(value: unknown): SystemParts | undefined {
  const content = text(value);
  if (!content) return;
  let scene = content, persona: string | undefined;
  if (content.startsWith('【职责与信息来源 ')) {
    const headers = [...content.matchAll(/^【(?:职责与信息来源|授权与行动边界|判断与结算|输出与投递|人格与风格) v1】\r?$/gm)];
    if (headers.length !== ruleHeadings.length || headers[0].index !== 0 ||
        !headers.every((header, i) => header[0].trimEnd() === ruleHeadings[i])) return;
    scene = content.slice(headers[0].index + headers[0][0].length, headers[1].index);
    const last = headers[4];
    persona = content.slice(last.index + last[0].length).replace(/^\n/, '');
  }
  const heading = scene.match(guidanceHeading);
  if (!heading || heading.index === undefined) return;
  const body = scene.slice(heading.index + heading[0].length).trim();
  // 新版领域 JSON 由插件独占一行发布，宿主可在其后追加本步协议。
  const guidance = decodedObject(body) ?? (persona !== undefined ? decodedObject(body.split('\n', 1)[0]) : undefined);
  if (guidance) return { title: heading[0].trim(), persona: persona ?? scene.slice(0, heading.index).trimEnd(), guidance };
}

export interface PromptDifference {
  readonly added: string[];
  readonly removed: string[];
  readonly changed: string[];
  readonly facts: string[];
  readonly messages: readonly { position: number; role: string; change: 'added' | 'removed' | 'changed' }[];
}

// 对照两次实际送模的工具定义与业务事实字段。
// 步骤：按原工具名称比较契约 → 对照 MC 事实 → 返回变动定位。
export function comparePrompts(before: Data, after: Data): PromptDifference {
  const beforeTools = toolDefinitions(before);
  const afterTools = toolDefinitions(after);
  const oldFacts = mcInput(before) ?? {};
  const newFacts = mcInput(after) ?? {};
  const facts = [...new Set([...Object.keys(oldFacts), ...Object.keys(newFacts)])]
    .filter(key => JSON.stringify(oldFacts[key]) !== JSON.stringify(newFacts[key]));
  return {
    added: [...afterTools.keys()].filter(tool => !beforeTools.has(tool)),
    removed: [...beforeTools.keys()].filter(tool => !afterTools.has(tool)),
    changed: [...afterTools].filter(([tool, value]) => beforeTools.has(tool) && beforeTools.get(tool) !== value).map(([tool]) => tool),
    facts,
    messages: changedMessages(objects(before.messages), objects(after.messages)),
  };
}

// 比较实际送模中的消息位置，以便回到两次原文核对。
function changedMessages(before: readonly Data[], after: readonly Data[]): PromptDifference['messages'] {
  const entries: PromptDifference['messages'][number][] = [];
  for (let index = 0; index < Math.max(before.length, after.length); index += 1) {
    const left = before[index];
    const right = after[index];
    if (JSON.stringify(left) === JSON.stringify(right)) continue;
    entries.push({ position: index + 1, role: text((right ?? left)?.role) ?? '未知角色',
      change: !left ? 'added' : !right ? 'removed' : 'changed' });
  }
  return entries;
}

// 以模型实际看到的名称索引同一送模快照的工具定义。
function toolDefinitions(snapshot: Data): Map<string, string> {
  const tools = new Map<string, string>();
  for (const entry of objects(snapshot.tools)) {
    if (typeof entry.name === 'string') tools.set(entry.name, JSON.stringify(entry));
  }
  return tools;
}

// 从终态诊断取出与所选送模或候选相关的程序裁定。
export function policyFacts(exit: Data | undefined, selected: ModelStage | undefined): Data[] {
  const timeline = object(object(object(exit?.diagnostic)?.data)?.timeline);
  return objects(object(timeline?.delta)?.facts).filter(fact => {
    if (fact.kind !== 'policy_decision') return false;
    if (!selected) return true;
    const subject = object(fact.subject);
    return id(subject?.step) === selected.step && id(subject?.attempt) === selected.attempt;
  });
}
