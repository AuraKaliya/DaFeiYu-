import { ref, shallowRef } from 'vue';

type Wire = Record<string, unknown>;
export interface StoredMessage {
  message: string; revision: string; utcMs: number; sender: string; route: string; text: string;
  source: string; submission: string; receipt: Wire; target?: Wire; sourceUtcMs?: number;
  instance?: string; session?: string; turn?: string; delivery?: string;
}
type Read = { kind: 'subscribe' | 'recent' | 'older' | 'changes'; id: string; cursor: string; through: string; sent: number };
const pageSize = 24;
const storageKey = 'dafeiyu.panel.history';

// 核对原生回执对象，保留原始字段类型。
function record(value: unknown): value is Wire { return !!value && typeof value === 'object' && !Array.isArray(value); }
// 保留 SQLite 十进制游标的精度，拒绝负数、溢出和非规范形式。
function cursor(value: unknown): value is string {
  return typeof value === 'string' && /^(0|[1-9]\d{0,18})$/.test(value) && BigInt(value) <= 9223372036854775807n;
}
// 核对正文、来源及真实时间，只将完整的消息页交给视图。
function stored(value: unknown): value is StoredMessage {
  return record(value) && cursor(value.message) && value.message !== '0' && cursor(value.revision) && value.revision !== '0' &&
    typeof value.utcMs === 'number' && Number.isSafeInteger(value.utcMs) && Math.abs(value.utcMs) <= 8.64e15 &&
    typeof value.text === 'string' && typeof value.sender === 'string' && typeof value.route === 'string' &&
    typeof value.source === 'string' && typeof value.submission === 'string' && record(value.receipt) &&
    (value.target === undefined || record(value.target));
}

// 一个窗口独立分页和追赶修订，刷新只记加载到的边界，正文始终从数据库重读。
// 步骤：订阅原实例并固定首屏水位 → 分页或补读 → 验证整页后提交游标及视图。
export function useMessageHistory() {
  const rows = shallowRef<readonly StoredMessage[]>([]);
  const instance = ref(''), conversation = ref(''), more = ref(false), loadingOlder = ref(false), error = ref('');
  const subscribed = ref(false);
  const records = new Map<string, StoredMessage>();
  const lookups = new Map<string, { source: string; submission: string; sent: number; consume?: (row: StoredMessage | null) => void }>();
  const prefix = 'history-' + crypto.randomUUID() + '-';
  let serial = 0, pending: Read | undefined, retryAt = 0;
  let loaded = false, wantOlder = false;
  let fixed = '0', oldest = '0', applied = '0', latest = '0', changingThrough = '0', restoreOldest = '0';

  // 读取上次成功展示的最早位置，同目录的其他逻辑会话不复用它。
  function restoredBoundary(identity: string): string {
    try {
      const saved: unknown = JSON.parse(sessionStorage.getItem(storageKey) ?? 'null');
      if (record(saved) && saved.conversation === identity && cursor(saved.oldest)) return saved.oldest;
    } catch { /* 首次打开或存储不可用时读取最新一页。 */ }
    return '0';
  }
  // 同一时间只发一项读取，迟到回包依请求身份丢弃。
  function request(kind: Read['kind'], index = '0', through = '0'): void {
    const id = prefix + ++serial;
    pending = { kind, id, cursor: index, through, sent: performance.now() };
    loadingOlder.value = kind === 'older';
    window.chrome.webview.postMessage(kind === 'subscribe' ? { type: 'messages.subscribe', version: '1', request: id } :
      { type: 'messages.read', version: '1', request: id, conversation: conversation.value,
        direction: kind === 'changes' ? 'changes' : 'history', cursor: index, through, limit: pageSize });
  }
  // 手动翻页优先于后台补读，同一批变化续页时保持其上界。
  function drive(): void {
    if (pending || performance.now() < retryAt) return;
    if (!subscribed.value) request('subscribe');
    else if (!loaded) request('recent', '0', fixed);
    else if (more.value && (wantOlder || (restoreOldest !== '0' && BigInt(oldest) > BigInt(restoreOldest)))) request('older', oldest, fixed);
    else if (BigInt(latest) > BigInt(applied)) {
      if (changingThrough === '0') changingThrough = latest;
      request('changes', applied, changingThrough);
    }
  }
  // 原连接失联时保留已读消息，下次先核验实例与提交水位。
  function disconnect(): void { pending = undefined; subscribed.value = false; lookups.clear(); loadingOlder.value = false; }
  // 失败与超时从实际已应用的游标重试，保留用户正在翻阅的位置。
  function failed(): void {
    disconnect();
    error.value = '聊天记录暂未读回';
    retryAt = performance.now() + 1200;
  }
  // 正式对话轮询提示新提交，同一水位不会反复读取正文。
  function pulse(watermark: unknown): void {
    if (pending && performance.now() - pending.sent >= 3500) failed();
    for (const [id, lookup] of lookups) if (performance.now() - lookup.sent >= 3500) lookups.delete(id);
    if (subscribed.value && cursor(watermark) && BigInt(watermark) > BigInt(latest)) latest = watermark;
    drive();
  }
  // 同一来源和提交号同时只发一次查询，补投与手动复查均使用原消息身份。
  function receipt(source: string, submission: string, consume?: (row: StoredMessage | null) => void): void {
    if (!subscribed.value || Array.from(lookups.values()).some(item => item.source === source && item.submission === submission)) return;
    const id = prefix + ++serial;
    lookups.set(id, { source, submission, sent: performance.now(), consume });
    window.chrome.webview.postMessage({ type: 'messages.receipt', version: '1', request: id, conversation: conversation.value, source, submission });
  }
  // 按持久号排序，只展示已经加载到的历史范围。
  function publishRows(): void {
    rows.value = Array.from(records.values()).filter(row => oldest === '0' || BigInt(row.message) >= BigInt(oldest))
      .sort((a, b) => BigInt(a.message) < BigInt(b.message) ? -1 : 1);
  }
  // 验证订阅与整页的固定上界、方向和顺序，再按消息号合并最新修订。
  // 步骤：核对原请求 → 绑定或复核逻辑会话 → 合并有效页并推进对应游标。
  function receive(value: unknown): boolean {
    if (!record(value) || typeof value.request !== 'string' || !value.request.startsWith(prefix)) return false;
    const lookup = lookups.get(value.request);
    if (lookup) {
      lookups.delete(value.request);
      if (value.version !== '1' || value.event !== 'messages.receipt' || value.instance !== instance.value ||
          value.conversation !== conversation.value) return true;
      const row = value.record;
      if (row === null) lookup.consume?.(null);
      else if (stored(row) && row.source === lookup.source && row.submission === lookup.submission) {
        const previous = records.get(row.message);
        if (!previous || BigInt(previous.revision) < BigInt(row.revision)) { records.set(row.message, row); publishRows(); }
        lookup.consume?.(row);
      }
      return true;
    }
    const read = pending;
    if (!read || read.id !== value.request) return true;
    pending = undefined;
    loadingOlder.value = false;
    if (value.version !== '1' || typeof value.conversation !== 'string' || !value.conversation ||
        typeof value.instance !== 'string' || !value.instance || !cursor(value.watermark)) { failed(); return true; }
    if (read.kind === 'subscribe' && value.event === 'messages.subscribed') {
      if (conversation.value !== value.conversation || BigInt(value.watermark) < BigInt(applied)) {
        records.clear(); rows.value = []; loaded = more.value = wantOlder = false;
        oldest = applied = '0';
        restoreOldest = restoredBoundary(value.conversation);
      }
      instance.value = value.instance; conversation.value = value.conversation; subscribed.value = true;
      latest = value.watermark; changingThrough = '0';
      if (!loaded) fixed = latest;
    } else {
      if (read.kind === 'subscribe' || value.event !== 'messages.page' || value.instance !== instance.value ||
          value.conversation !== conversation.value || value.through !== read.through || !cursor(value.next) ||
          !Array.isArray(value.records) || value.records.length > pageSize || !value.records.every(stored) ||
          typeof value.more !== 'boolean' || (value.more && !value.records.length)) { failed(); return true; }
      let next = read.cursor;
      const changes = read.kind === 'changes';
      for (const row of value.records) {
        const position = changes ? row.revision : row.message;
        if (BigInt(row.revision) > BigInt(read.through) || (changes ? BigInt(position) <= BigInt(next) :
            next !== '0' && BigInt(position) >= BigInt(next))) { failed(); return true; }
        next = position;
      }
      if (value.next !== next) { failed(); return true; }
      for (const row of value.records) {
        const previous = records.get(row.message);
        if (!previous || BigInt(previous.revision) < BigInt(row.revision)) records.set(row.message, row);
      }
      if (changes) {
        applied = value.more ? next : read.through;
        if (!value.more) changingThrough = '0';
      } else {
        oldest = next; more.value = value.more; wantOlder = false;
        if (read.kind === 'recent') { loaded = true; applied = read.through; }
        if (!more.value || BigInt(oldest) <= BigInt(restoreOldest) || restoreOldest === '0') {
          restoreOldest = '0';
          try { sessionStorage.setItem(storageKey, JSON.stringify({ conversation: conversation.value, oldest })); } catch { /* 正文仍可从原水位读取。 */ }
        }
      }
      publishRows();
    }
    error.value = '';
    drive();
    return true;
  }
  // 用户显式翻阅时保留当前视图，收到上一页后再更新最早位置。
  function earlier(): void { wantOlder = true; drive(); }
  // 用户重试可以直接恢复订阅读取。
  function retry(): void { retryAt = 0; drive(); }
  return { rows, instance, conversation, subscribed, more, loadingOlder, error, earlier, retry, disconnect, receive, pulse, receipt };
}
