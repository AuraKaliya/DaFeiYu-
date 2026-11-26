import type { StoredMessage } from './message-history';

type StableSubmission = { submission?: string; sourceUtcMs?: number };
export type TextCommand = StableSubmission & { command: string; text: string } &
  ({ type: 'dialogue.submit' } | { type: 'dialogue.append' | 'dialogue.enqueue'; session: string; turn: string; instance?: string });
export type WireCommand = TextCommand |
  { type: 'dialogue.cancel' | 'dialogue.clear'; command: string; session: string; turn: string } |
  { type: 'dialogue.resume' | 'dialogue.remove'; command: string; session: string; message: string };
const forms = {
  'dialogue.submit': ['text'], 'dialogue.append': ['session', 'turn', 'text'], 'dialogue.enqueue': ['session', 'turn', 'text'],
  'dialogue.cancel': ['session', 'turn'], 'dialogue.clear': ['session', 'turn'],
  'dialogue.resume': ['session', 'message'], 'dialogue.remove': ['session', 'message'],
} as const;
const encoder = new TextEncoder();
const limit = 1024 * 1024;
const draftKey = 'dafeiyu.panel.draft';

// 核对同一历史项留下的命令形状，保留原参数与十进制身份。
function isWireCommand(value: unknown): value is WireCommand {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const item = value as Record<string, unknown>;
  if (typeof item.type !== 'string' || !Object.hasOwn(forms, item.type)) return false;
  const keys = [...forms[item.type as keyof typeof forms], 'command'];
  const allowed = new Set<string>(['type', ...keys]);
  if (keys.includes('text') && item.submission !== undefined) {
    if (typeof item.submission !== 'string' || !item.submission || encoder.encode(item.submission).length > 128) return false;
    allowed.add('submission');
    if (item.sourceUtcMs !== undefined &&
        (typeof item.sourceUtcMs !== 'number' || !Number.isSafeInteger(item.sourceUtcMs) || item.sourceUtcMs < 0)) return false;
    allowed.add('sourceUtcMs');
    if (item.type !== 'dialogue.submit') {
      if (typeof item.instance !== 'string' || !item.instance) return false;
      allowed.add('instance');
    }
  }
  return Object.keys(item).every(key => allowed.has(key)) && keys.every(key => {
    const part = item[key];
    return typeof part === 'string' && (key === 'text' ? !!part.trim() && encoder.encode(part).length <= limit :
      /^[1-9]\d{0,19}$/.test(part) && BigInt(part) <= 18446744073709551615n);
  });
}

// 读取当前面板历史项中的草稿和至多两个未决意图，接纳状态仍须向原生端查询。
export function recoverDialogue(): { draft: string; commands: WireCommand[]; conversation: string } {
  const saved = history.state?.dialogue;
  let draft = typeof saved?.draft === 'string' ? saved.draft : '';
  try { draft = sessionStorage.getItem(draftKey) ?? draft; } catch { /* 仍可使用原命令历史项中的草稿。 */ }
  if (draft.length > limit) draft = '';
  const commands = saved?.commands;
  return { draft, conversation: typeof saved?.conversation === 'string' ? saved.conversation : '',
    commands: Array.isArray(commands) && commands.length <= 2 && commands.every(isWireCommand) ? commands : [] };
}

// 原提交的载荷和目标一致才可用持久回执结算草稿，未决状态继续查询原记录。
export function originalReceipt(fields: TextCommand, row: StoredMessage) {
  const route = fields.type === 'dialogue.submit' ? 'send' : fields.type === 'dialogue.append' ? 'append' : 'enqueue';
  if (row.source !== 'desktop' || row.submission !== fields.submission || row.sender !== 'user' ||
      row.route !== route || row.text !== fields.text || row.sourceUtcMs !== fields.sourceUtcMs ||
      (fields.type === 'dialogue.submit' ? row.target !== undefined : !row.target ||
        row.target.instance !== fields.instance || row.target.session !== fields.session || row.target.turn !== fields.turn)) return 'conflict';
  switch (row.receipt.state) {
    case 'pending': case 'dispatching': return 'pending';
    case 'accepted': case 'waiting': case 'queued': case 'submitted': return 'accepted';
    case 'full': case 'limit': return 'full';
    case 'stale': case 'closed': case 'removed': return 'stale';
    case 'unknown': case 'unavailable': return 'unavailable';
    default: return 'rejected';
  }
}

// 即时保存同一窗口、独立面板来源的草稿，导航开始后历史项可能已无法修改。
export function rememberDraft(draft: string): void { sessionStorage.setItem(draftKey, draft); }

// 保存本窗口当前历史项，页面重载后沿同一命令核对，窗口关闭后随独立配置回收。
export function rememberDialogue(draft: string, commands: Iterable<WireCommand>, conversation: string): void {
  history.replaceState({ ...history.state, dialogue: { draft, conversation, commands: Array.from(commands) } }, '');
}
