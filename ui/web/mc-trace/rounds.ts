import { decodedObject, id, object, objects, scanFile, text } from './trace-file.ts';
import type { Data, LineRef, ScanResult } from './trace-file.ts';

const triggers: Readonly<Record<string, readonly [string, string]>> = {
  'game.event': ['游戏输入', '游戏输入'],
  'motion.finished': ['工作结算', '动作退出'],
  'plan.finished': ['工作结算', '计划结算'],
  'task.ready': ['工作结算', '任务交接'],
  'follow.attention': ['现场变化', '跟随需要关注'],
  'scene.changed': ['现场变化', '等待的场景变化'],
  'question.result': ['追问与审批', '追问有了结果'],
  'approval.result': ['追问与审批', '审批有了结果'],
  'task.continue': ['续行与能力', '任务继续'],
  'skills.expanded': ['续行与能力', '工具范围扩展'],
  'main.delegation': ['委托与复核', '主 Agent 委托'],
};

export interface Trigger {
  readonly code: string;
  readonly category: string;
  readonly label: string;
}

export interface GameFact {
  readonly sequence?: string;
  readonly type?: string;
  readonly text?: string;
}

export interface RoundInput {
  readonly trigger: Trigger;
  readonly generation: string;
  readonly facts: readonly GameFact[];
  readonly reason?: string;
  readonly related: Data;
}

export interface RecordEntry {
  readonly ref: LineRef;
  readonly kind: string;
  readonly time?: string;
  readonly key?: string;
  readonly step?: string;
  readonly attempt?: string;
  readonly request?: string;
  readonly candidate?: string;
  readonly invocation?: string;
  readonly callId?: string;
  readonly name?: string;
  readonly result?: string;
  readonly effect?: string;
  readonly fault?: string;
  readonly reason?: string;
  readonly business?: string;
  readonly timing?: Readonly<{ submitted?: string; started?: string; finished?: string }>;
}

export interface Round {
  readonly key: string;
  readonly session: string;
  readonly run: string;
  readonly accepted?: RecordEntry;
  readonly input?: RoundInput;
  readonly events: readonly RecordEntry[];
  readonly exit?: RecordEntry;
  readonly modelCount: number;
}

export interface HostIndex {
  readonly source: ScanResult;
  readonly records: readonly RecordEntry[];
  readonly rounds: readonly Round[];
  readonly utcMs?: number;
}

// 读取实际送模中的 MC 事实，不把同轮的其他消息当成新的触发。
export function mcInput(request: unknown): Data | undefined {
  for (const message of objects(object(request)?.messages)) {
    if (message.role !== 'user') continue;
    const facts = decodedObject(message.content);
    if (!text(facts?.generation)) continue;
    if ((typeof facts?.trigger === 'string' && Array.isArray(facts.events)) ||
        (Array.isArray(facts?.currentEvents) && Array.isArray(facts.newEvents) && object(facts.work))) return facts;
  }
}

// 以原生触发码归类已知回合，旧日志的动作复核使用自己的送模形态。
export function describeTrigger(input?: Data): Trigger {
  const code = text(input?.trigger);
  if (!code && Array.isArray(input?.newEvents) && object(input?.work))
    return { code: '', category: '委托与复核', label: '在途动作复核' };
  const known = code && Object.hasOwn(triggers, code) ? triggers[code] : undefined;
  if (known && code) return { code, category: known[0], label: known[1] };
  return { code: code ?? '', category: '触发待确认', label: '触发待确认' };
}

// 摘录用于列表和跨回合关联的事实，长上下文留在原文件内。
// 步骤：分类触发 → 保存玩家事实与原因 → 摘录后续关联所需的原票据。
function compactInput(facts: Data): RoundInput {
  const attention = object(facts.followAttention);
  const plan = object(facts.taskPlan);
  const settlement = object(facts.workSettlement);
  const task = object(facts.task);
  const taskClarification = object(task?.clarification);
  return {
    trigger: describeTrigger(facts),
    generation: text(facts.generation) ?? '',
    facts: objects(facts.events ?? facts.newEvents).map(entry => ({
      sequence: id(entry.sequence), type: text(entry.type), text: text(entry.text)?.slice(0, 250),
    })),
    reason: text(attention?.reason) ?? text(object(facts.questionResult)?.status) ?? text(object(facts.approvalResult)?.status),
    related: {
      plan: plan?.id,
      origin: object(plan?.origin)?.sequence ?? object(taskClarification?.origin)?.sequence,
      plans: objects(facts.planSettlements).map(entry => entry.plan),
      works: [...objects(facts.planSettlements).flatMap(layer => objects(layer.nodes).map(node => node.work)),
        ...objects(settlement?.receipts).map(receipt => receipt.bridgeWork), object(facts.taskHandoff)?.work],
      question: object(facts.questionResult)?.item ?? object(facts.pendingQuestion)?.item ??
        object(plan?.clarification)?.item ?? taskClarification?.item,
      approval: object(facts.approvalResult)?.request ?? object(facts.pendingApproval)?.request,
      follow: object(facts.following)?.work,
      episode: attention?.episode,
      reviewed: object(facts.work)?.candidate,
      delegation: object(facts.delegation)?.id,
    },
  };
}

// 取出可索引的回合、调用和计时字段，正文由原行提供。
// 步骤：核对会话身份 → 摘录模型与工具状态 → 保留退出原因。
function project(row: Data, ref: LineRef): RecordEntry {
  const kind = text(row.kind) ?? '';
  const session = id(row.session);
  const run = id(row.run);
  const output = kind === 'tool.exited' ? decodedObject(row.output) : undefined;
  const timeline = object(object(object(row.diagnostic)?.data)?.timeline);
  return {
    ref, kind, time: id(row.timeNs), key: session && run ? `${session}/${run}` : undefined,
    step: id(row.step), attempt: id(row.attempt), request: id(row.request), candidate: id(row.candidate),
    invocation: id(row.invocation), callId: text(row.callId), name: text(row.toolId) ?? text(row.name),
    result: text(row.result), effect: text(row.effect), fault: text(row.fault), business: text(output?.status),
    reason: text(output?.reason) ?? text(output?.reasonCode) ??
      (timeline?.stop_reason === 'none' ? undefined : text(timeline?.stop_reason)) ??
      (row.error === 'none' ? undefined : text(row.error)),
    timing: kind.startsWith('model.') ?
      { submitted: id(row.submittedNs), started: id(row.startedNs), finished: id(row.finishedNs) } : undefined,
  };
}

interface RoundDraft {
  readonly key: string;
  readonly session: string;
  readonly run: string;
  readonly events: RecordEntry[];
  plugin?: string;
  accepted?: RecordEntry;
  input?: RoundInput;
  exit?: RecordEntry;
}

// 索引宿主证据和实际 MC 回合，允许接纳记录出现在首次准备之后。
// 步骤：流式投影原行 → 汇合独立子会话 → 按首条记录输出分页。
export async function indexHost(
  file: Blob, onRecord?: (row: Data, ref: LineRef, entry: RecordEntry) => void, progress?: (bytes: number) => void,
): Promise<HostIndex> {
  const records: RecordEntry[] = [];
  const drafts = new Map<string, RoundDraft>();
  let utcMs: number | undefined;
  const source = await scanFile(file, 'trace', (row, ref) => {
    const entry = project(row, ref);
    records.push(entry);
    if (entry.kind === 'trace.open' && typeof row.utcMs === 'number') utcMs = row.utcMs;
    const session = id(row.session);
    const run = id(row.run);
    if (entry.key && session && run && (row.source === 'plugin' || entry.kind === 'subagent.accepted')) {
      let draft = drafts.get(entry.key);
      if (!draft) {
        draft = { key: entry.key, session, run, events: [] };
        drafts.set(entry.key, draft);
      }
      draft.events.push(entry);
      if (entry.kind === 'subagent.accepted') {
        draft.plugin = text(row.plugin);
        if (draft.plugin === 'mc.companion') draft.accepted = entry;
      }
      if (entry.kind === 'run.exited') draft.exit = entry;
      if (!draft.input && entry.kind === 'model.prepared') {
        const input = mcInput(row.input);
        if (input) draft.input = compactInput(input);
      }
    }
    onRecord?.(row, ref, entry);
  }, progress);
  const rounds = Array.from(drafts.values(), (draft): Round | undefined => {
    if (!draft.accepted && (!draft.input || draft.plugin)) return undefined;
    return { key: draft.key, session: draft.session, run: draft.run, accepted: draft.accepted, input: draft.input,
      events: draft.events, exit: draft.exit, modelCount: draft.events.filter(entry => entry.kind === 'model.submitted').length };
  }).filter(draft => draft !== undefined);
  return { source, records, rounds, utcMs };
}
