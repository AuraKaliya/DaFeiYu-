import { decodedObject, object, objects, text } from './trace-file.ts';
import type { Data } from './trace-file.ts';

type Identity = string | number;

interface DecisionStatus {
  readonly snapshot: Identity;
  readonly turn: Identity;
  readonly step: Identity;
  readonly scope: Data;
  readonly asOf: Data;
  readonly wakeEvents: readonly unknown[];
  readonly items: readonly Data[];
}

export interface PreparedMessage {
  readonly position: number;
  readonly message: Data;
  readonly source?: string;
  readonly eventUtcMs?: Identity;
  readonly status?: DecisionStatus;
}

// 保留日志中的文本编号及可精确表示的数字，零也是有效记录。
function identity(value: unknown): value is Identity {
  return (typeof value === 'string' && value.length > 0) || (typeof value === 'number' && Number.isSafeInteger(value));
}

// 读取已由原消息来源确认的 v1 宿主尾注，残缺正文仍由原文视图保留。
// 步骤：核对格式与作用域 → 检查每项原件及值 → 返回该次决策的实际记录。
function decisionStatus(content: unknown): DecisionStatus | undefined {
  if (typeof content !== 'string') return;
  const note = decodedObject(content);
  if (note?.schema !== 'dafeiyu.agent_status/1' || note.source !== 'host') return;
  const scope = object(note.scope);
  const asOf = object(note.asOf);
  if (!scope || !identity(scope.session) || !identity(scope.epoch) || !identity(note.snapshot) ||
      !identity(note.turn) || !identity(note.step) || !asOf || !Array.isArray(note.wakeEvents) || !Array.isArray(note.items)) return;
  const items = objects(note.items);
  if (items.length !== note.items.length || items.some(item => !text(item.key) || !identity(item.version) ||
      !text(item.source) || !Object.hasOwn(item, 'subject') || !Object.hasOwn(item, 'fact') || !Object.hasOwn(item, 'value'))) return;
  return { snapshot:note.snapshot, turn:note.turn, step:note.step, scope, asOf, wakeEvents:note.wakeEvents, items };
}

// 按同一条装配原行的物理下标取来源，仅将最后一条可信 user 正文投影为本步快照。
// 步骤：核对并行数组长度 → 保留每条原消息的位置 → 对末条宿主状态解码。
export function preparedMessages(row?: Data): PreparedMessage[] {
  const messages = object(row?.input)?.messages;
  if (row?.kind !== 'model.prepared' || !Array.isArray(messages)) return [];
  const evidence = Array.isArray(row.messageEvidence) && row.messageEvidence.length === messages.length ? row.messageEvidence : undefined;
  const result: PreparedMessage[] = [];
  for (const [index, raw] of messages.entries()) {
    const message = object(raw);
    if (!message) continue;
    const origin = object(evidence?.[index]);
    const source = text(origin?.source);
    const eventUtcMs = origin?.eventUtcMs;
    const tail = index === messages.length - 1 && message.role === 'user' && source === 'host_status';
    result.push({ position:index + 1, message, source, eventUtcMs:identity(eventUtcMs) ? eventUtcMs : undefined,
      status:tail ? decisionStatus(message.content) : undefined });
  }
  return result;
}
