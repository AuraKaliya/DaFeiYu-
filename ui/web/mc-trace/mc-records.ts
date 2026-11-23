import { decodedObject, id, object, objects, scanFile, text } from './trace-file.ts';
import type { Data, LineRef, ScanResult } from './trace-file.ts';
import type { RecordEntry } from './rounds.ts';

export type McKind = 'event' | 'turn' | 'motion' | 'action' | 'plan' | 'queue' | 'attention' |
  'question' | 'approval' | 'delegation' | 'control' | 'game';

const categories: Readonly<Record<string, McKind>> = {
  'mc.partner.event': 'event', 'mc.partner.turn': 'turn', 'mc.partner.motion': 'motion',
  'mc.partner.action': 'action', 'mc.partner.plan': 'plan', 'mc.partner.queue': 'queue',
  'mc.partner.attention': 'attention', 'mc.partner.question': 'question', 'mc.partner.report': 'delegation',
  'mc.partner.control': 'control',
};

export interface McRecord {
  readonly kind: McKind;
  readonly code: string;
  readonly ref: LineRef;
  readonly time?: string;
  readonly generation?: string;
  readonly session?: string;
  readonly request?: string;
  readonly invocation?: string;
  readonly order?: string;
  readonly sourceOrder?: string;
  readonly answerOrder?: string;
  readonly plan?: string;
  readonly work?: string;
  readonly follow?: string;
  readonly episode?: string;
  readonly workItems: readonly string[];
  readonly item?: string;
  readonly ask?: string;
  readonly delegation?: string;
  readonly skill?: string;
  readonly status?: string;
  readonly reason?: string;
  readonly effect?: string;
  readonly delivery?: string;
  readonly summary?: string;
  readonly changes?: number;
}

// 从原生回执读取送达状态，保留未知、失败和实际发言的不同证据。
function deliveryStatus(result?: Data) {
  if (result?.sent === true) return 'sent';
  return text(result?.delivery);
}

// 保留日志中的工作号列表，缺失条目留由该记录的原文解释。
function workIds(nodes: unknown): string[] {
  return objects(nodes).map(node => text(node.work)).filter(work => work !== undefined);
}

// 从信箱及 ASK 事件中摘录关联身份和可浏览摘要。
// 步骤：识别领域事件 → 解开嵌套回执 → 保留游戏和宿主各自身份。
export function projectMc(row: Data, entry: RecordEntry): McRecord | undefined {
  const code = text(row.category) ?? entry.kind;
  const type = Object.hasOwn(categories, code) ? categories[code] :
    code.startsWith('mc.confirmation.') ? 'approval' :
    code.startsWith('ask.') || code.startsWith('interaction.') ? 'question' : undefined;
  if (!type) return undefined;
  const body = decodedObject(row.body) ?? (row.category === undefined ? row : undefined);
  if (!body) return undefined;
  const action = object(body.action) ?? body;
  const event = object(body.event);
  const plan = object(body.plan);
  const result = object(action.result) ?? object(body.result);
  const effect = typeof action.effect === 'number' ? effectNames[action.effect] : text(action.effect);
  return {
    kind: type, code, ref: entry.ref, time: entry.time,
    generation: text(action.generation) ?? text(body.generation), session: id(action.session), request: id(action.request),
    invocation: id(action.invocation) ?? id(result?.attempt) ?? (type === 'approval' ? id(body.id) : undefined),
    order: id(action.order) ?? id(event?.sequence), sourceOrder: id(body.sourceMessage),
    answerOrder: id(body.answerMessage), plan: id(plan?.id ?? body.plan),
    work: text(action.bridgeWork) ?? (type === 'turn' ? text(body.closingReply) : undefined),
    follow: text(action.work), episode: id(body.episode), workItems: [
      ...workIds(plan?.nodes ?? body.nodes),
      ...(Array.isArray(body.works) ? body.works.map(text).filter(work => work !== undefined) : []),
    ],
    item: id(body.item), ask: id(body.ask), delegation: type === 'delegation' ? id(body.id) : undefined,
    skill: text(action.skill),
    status: text(body.status) ?? text(result?.status) ?? text(body.outcome) ?? text(event?.type) ?? text(body.phase),
    reason: text(result?.reason) ?? text(result?.reasonCode) ?? text(body.reason) ?? text(body.previousReason),
    effect, delivery: deliveryStatus(result),
    summary: (text(event?.text) ?? text(result?.text) ?? text(body.answer) ?? text(body.text) ?? text(body.question))?.slice(0, 220),
    changes: Array.isArray(result?.changes) ? result.changes.length : undefined,
  };
}

const effectNames: Readonly<Record<number, string>> = { 1: 'not_applied', 2: 'applied', 3: 'unknown' };

// 读取独立桥接日志的实际退出和效果，保留行顺序及原工作号。
// 步骤：流式读回 → 摘录工单和状态 → 记录文件完整性。
export async function indexGame(file: Blob, progress?: (bytes: number) => void): Promise<{ source: ScanResult; records: McRecord[] }> {
  const records: McRecord[] = [];
  const source = await scanFile(file, 'game', (row, ref) => {
    const result = object(row.result);
    records.push({
      kind: 'game', code: 'game-actions.jsonl', ref, generation: text(row.generation), work: text(row.work),
      order: id(row.order), workItems: [], skill: text(row.skill),
      status: text(result?.status) ?? (row.exited === true ? 'exited' : undefined),
      reason: text(result?.reason) ?? text(result?.reasonCode), delivery: deliveryStatus(result),
      summary: text(result?.text)?.slice(0, 220),
      changes: Array.isArray(result?.changes) ? result.changes.length : undefined,
    });
  }, progress);
  return { source, records };
}
