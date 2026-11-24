export type IntentAction = 'send' | 'append' | 'enqueue' | 'cancel' | 'clear' | 'resume' | 'remove';
export type WireCommand = Readonly<Record<string, string>>;
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
  return Object.keys(item).length === keys.length + 1 && keys.every(key => {
    const part = item[key];
    return typeof part === 'string' && (key === 'text' ? !!part.trim() && encoder.encode(part).length <= limit :
      /^[1-9]\d{0,19}$/.test(part) && BigInt(part) <= 18446744073709551615n);
  });
}

// 读取当前面板历史项中的草稿和至多两个未决意图，接纳状态仍须向原生端查询。
export function recoverDialogue(): { draft: string; commands: WireCommand[] } {
  const saved = history.state?.dialogue;
  let draft = typeof saved?.draft === 'string' ? saved.draft : '';
  try { draft = sessionStorage.getItem(draftKey) ?? draft; } catch { /* 仍可使用原命令历史项中的草稿。 */ }
  if (draft.length > limit) draft = '';
  const commands = saved?.commands;
  return { draft, commands: Array.isArray(commands) && commands.length <= 2 && commands.every(isWireCommand) ? commands : [] };
}

// 即时保存同一窗口、独立面板来源的草稿，导航开始后历史项可能已无法修改。
export function rememberDraft(draft: string): void { sessionStorage.setItem(draftKey, draft); }

// 保存本窗口当前历史项，页面重载后沿同一命令核对，窗口关闭后随独立配置回收。
export function rememberDialogue(draft: string, commands: Iterable<WireCommand>): void {
  history.replaceState({ dialogue: { draft, commands: Array.from(commands) } }, '');
}
