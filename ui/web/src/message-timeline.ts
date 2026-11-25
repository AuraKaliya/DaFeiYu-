import type { StoredMessage } from './message-history';

interface SnapshotTurn {
  session: string; turn: string; queuedMessage: string; inputs: readonly { message: string }[];
}
interface Live<T> {
  key: string; turn: T; order?: bigint; prompt?: StoredMessage; answer?: StoredMessage; inputs: Map<string, StoredMessage>;
}
type Entry<T> = Live<T> | { key: string; row: StoredMessage; order: bigint };

// 以原实例和业务身份合并已提交正文与最近回合的控制状态，同文的独立提交各保留一行。
// 步骤：索引当前回合 → 按发送、补充、出队与正式回答关联 → 依持久号插入独立历史消息。
export function messageTimeline<T extends SnapshotTurn>(rows: readonly StoredMessage[], turns: readonly T[], instance: string): Entry<T>[] {
  const live = turns.map((turn): Live<T> => ({
    key: 'turn-' + turn.session + '-' + turn.turn, turn, inputs: new Map(),
  }));
  const byRun = new Map(live.map(entry => [entry.turn.session + '/' + entry.turn.turn, entry]));
  const entries: Entry<T>[] = [];
  for (const row of rows) {
    const binding = row.sender === 'assistant' ? row : row.receipt;
    const entry = binding.instance === instance ? byRun.get(binding.session + '/' + binding.turn) : undefined;
    if (entry) {
      const native = row.receipt.nativeMessage;
      if (row.sender === 'user' && row.receipt.state === 'accepted' &&
          (row.route === 'send' || (row.route === 'enqueue' && native === entry.turn.queuedMessage))) {
        entry.prompt = row; entry.order = BigInt(row.message); continue;
      }
      if (row.sender === 'user' && row.route === 'append' && typeof native === 'string' &&
          entry.turn.inputs.some(input => input.message === native)) { entry.inputs.set(native, row); continue; }
      if (row.sender === 'assistant' && row.delivery === [instance, entry.turn.session, entry.turn.turn, 'final'].join('/')) {
        entry.answer = row; entry.order ??= BigInt(row.message); continue;
      }
    }
    entries.push({ key: 'message-' + row.message, row, order: BigInt(row.message) });
  }
  // 未载入旧页的近期回合沿用宿主顺序，新回合在取得持久号前排到末尾。
  let nextOrder: bigint | undefined;
  for (let index = live.length - 1; index >= 0; --index) {
    const entry = live[index];
    entry.order ??= nextOrder;
    nextOrder = entry.order;
  }
  return [...entries, ...live].sort((a, b) => a.order === b.order ? 0 :
    a.order === undefined ? 1 : b.order === undefined || a.order < b.order ? -1 : 1);
}
