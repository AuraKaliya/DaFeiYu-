import { shallowRef } from 'vue';
import type { MailboxView, MailDirectory, MailSnapshot, MailSource, MailLetter } from './mailbox-view';

type Wire = Record<string, unknown>;
type Kind = 'list' | 'read';
type Request = { id: string; instance: string; source: string; sent: number };
type Post = (fields: Readonly<Record<string, string>>) => void;

// 核对管理回包的对象边界。
function object(value: unknown): value is Wire { return value !== null && typeof value === 'object' && !Array.isArray(value); }
// 来源、实例、信件号和修订全部保留为字符串。
function id(value: unknown): value is string { return typeof value === 'string' && !!value; }
// 只展示宿主实际采样的 UTC 毫秒时间。
function sampled(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isSafeInteger(value) && Math.abs(value) <= 8.64e15 ? value : undefined;
}
// 同一实例内，十进制修订保持原有精度并拒绝倒退。
function older(next: string, current?: string): boolean {
  return !!current && /^\d+$/.test(next) && /^\d+$/.test(current) && BigInt(next) < BigInt(current);
}
// 将快照内唯一身份的公开项目逐项投影，拒绝部分损坏的整份读数。
function items<T extends { id: string }>(raw: unknown, parse: (entry: Wire) => T | undefined): T[] | undefined {
  if (!Array.isArray(raw)) return;
  const result: T[] = [], seen = new Set<string>();
  for (const entry of raw) {
    const item = object(entry) ? parse(entry) : undefined;
    if (!item || seen.has(item.id)) return;
    seen.add(item.id);
    result.push(item);
  }
  return result;
}
// 目录仅采用本体提供的来源身份和名称。
function source(entry: Wire): MailSource | undefined {
  if (id(entry.id) && id(entry.label)) return { id: entry.id, label: entry.label,
    kind: id(entry.kind) ? entry.kind : undefined, generation: id(entry.generation) ? entry.generation : undefined,
    plugin: id(entry.plugin) ? entry.plugin : undefined };
}
// 只选择管理协议公开的字段，未知字段留在原回包中。
function letter(entry: Wire): MailLetter | undefined {
  if (!id(entry.id) || !id(entry.category) || (entry.state !== 'pending' && entry.state !== 'received')) return;
  return { id: entry.id, category: entry.category, state: entry.state, receipts: [] };
}

// 每个窗口持有独立的信箱观察范围，后台拥有消费游标。
// 步骤：读取来源目录 → 按原实例和来源订正快照 → 失联或切换时使旧请求失效。
export function useMailboxObserver(post: Post, clock = () => performance.now()) {
  const view = shallowRef<MailboxView>({ directoryStatus: 'loading', source: '', sourceLabel: '', detailStatus: 'loading' });
  const prefix = 'mail-' + crypto.randomUUID() + '-';
  const pending: Partial<Record<Kind, Request>> = {};
  const due = { list: 0, read: 0 };
  let sequence = 0, online = false, active = false, open = false, discover = true;

  // 原件失联时保留最近读数，不把网络失败显示为空信箱。
  function fail(kind: Kind): void {
    delete pending[kind];
    due[kind] = clock() + 1500;
    if (kind === 'list') {
      delete pending.read;
      discover = true;
      view.value = { ...view.value, directoryStatus: 'unavailable',
        detailStatus: view.value.detailStatus === 'stale' ? 'stale' : 'unavailable' };
    } else view.value = { ...view.value, detailStatus: 'unavailable' };
  }
  // 发出有界的只读请求，保存请求当时的完整观察身份。
  function send(kind: Kind): void {
    const instance = kind === 'list' && discover ? '' : view.value.directory?.instance ?? '';
    const source = kind === 'read' ? view.value.source : '';
    const request = prefix + ++sequence;
    pending[kind] = { id: request, instance, source, sent: clock() };
    const fields: Record<string,string> = { version: '1', request, type: 'mailbox.' + kind };
    if (instance) fields.instance = instance;
    if (source) fields.source = source;
    try { post(fields); } catch { fail(kind); }
  }
  // 目录和可见详情分别限频，每种读取至多一个在途请求。
  function tick(): void {
    if (!active || !online) return;
    const now = clock();
    for (const kind of ['list','read'] as const) {
      const current = pending[kind];
      if (current && now - current.sent >= 3500) fail(kind);
    }
    if (!pending.list && now >= due.list) send('list');
    if (open && view.value.source && view.value.directoryStatus === 'ready' && view.value.detailStatus !== 'stale' &&
        !pending.read && now >= due.read) send('read');
  }
  // 连接或房间可见性改变后重新核实目录，其他窗口的请求独立。
  function context(connected: boolean, visible: boolean): void {
    if (connected === online && visible === active) return;
    online = connected; active = visible; discover = true;
    delete pending.list; delete pending.read;
    due.list = due.read = 0;
    view.value = { ...view.value, directoryStatus: !online && view.value.directory ? 'stale' : online ? 'loading' : 'unavailable',
      detailStatus: view.value.detailStatus === 'stale' ? 'stale' : online ? 'loading' : 'unavailable' };
    tick();
  }
  // 展开后重新取原来源的详情，收起即废弃在途的详情回包。
  function visibility(visible: boolean): void {
    open = visible; delete pending.read; due.read = 0;
    if (open) { due.list = 0; tick(); }
  }
  // 用户显式切换才绑定新来源，相同名称的重载来源不会继承旧信件。
  function select(selected: string): void {
    const current = view.value.directory?.sources.find(item => item.id === selected);
    if (selected && !current) return;
    delete pending.read; due.read = 0;
    view.value = { ...view.value, source: selected, sourceLabel: current?.label ?? '', snapshot: undefined,
      selectionNotice: '', detailStatus: online && view.value.directoryStatus === 'ready' ? 'loading' : 'unavailable' };
    tick();
  }
  // 用户重试同一观察范围，已有请求仍按原编号完成。
  function retry(): void { due.list = due.read = 0; tick(); }
  // 先核对目录实例再处理来源消失，旧实例的详情和标签一起归还。
  function directory(next: MailDirectory): void {
    const old = view.value;
    if (old.directory && old.directory.instance !== next.instance) {
      delete pending.read;
      view.value = { directory: next, directoryStatus: 'ready', source: '', sourceLabel: '', detailStatus: 'loading',
        selectionNotice: old.source ? '后台实例已更换，请重新选择来源。' : '' };
    } else {
      if (older(next.revision,old.directory?.revision)) { fail('list'); return; }
      const item = next.sources.find(entry => entry.id === old.source);
      const former = old.directory?.sources.find(entry => entry.id === old.source);
      if (item && former?.generation && item.generation && former.generation !== item.generation) {
        delete pending.read;
        view.value = { directory: next, directoryStatus: 'ready', source: '', sourceLabel: '', detailStatus: 'loading',
          selectionNotice: '原来源已失效，请重新选择加载后的来源。' };
        discover = false;
        return;
      }
      view.value = { ...old, directory: next, directoryStatus: 'ready', sourceLabel: item?.label ?? old.sourceLabel };
      if (old.source && !item) {
        delete pending.read;
        view.value = { ...view.value, snapshot: undefined, detailStatus: 'stale' };
      }
    }
    discover = false;
  }
  // 只有原文档、原请求和原来源的有效回包能更新这份窗口视图。
  // 步骤：定位请求 → 核对信封和单项身份 → 整批更新或标记读取失败。
  function receive(value: unknown): void {
    if (!object(value) || typeof value.request !== 'string' || !value.request.startsWith(prefix)) return;
    const kind = pending.list?.id === value.request ? 'list' : pending.read?.id === value.request ? 'read' : undefined;
    if (!kind) return;
    const request = pending[kind]!;
    delete pending[kind];
    due[kind] = clock() + 1500;
    if (value.version !== '1' || !id(value.instance) || !id(value.revision)) { fail(kind); return; }
    if (kind === 'list' && value.event === 'mailbox.directory' && value.state === 'stale') {
      delete pending.read; discover = true;
      view.value = { ...view.value, directoryStatus: 'stale', detailStatus: 'stale', snapshot: undefined };
      return;
    }
    if (request.instance && value.instance !== request.instance) { fail(kind); return; }
    if (kind === 'list' && value.event === 'mailbox.directory') {
      const sources = items(value.sources,source);
      if (!sources || (value.state !== undefined && value.state !== 'ready')) { fail(kind); return; }
      directory({ instance: value.instance, revision: value.revision, sampledUtcMs: sampled(value.sampledUtcMs), sources });
    } else if (kind === 'read' && value.event === 'mailbox.snapshot' && value.source === request.source &&
        view.value.source === request.source && (value.state === 'ready' || value.state === 'stale')) {
      const messages = items(value.messages,letter);
      if (!messages || (value.state === 'ready' && older(value.revision, view.value.snapshot?.revision))) { fail(kind); return; }
      const snapshot: MailSnapshot = { instance: value.instance, source: request.source, revision: value.revision,
        sampledUtcMs: sampled(value.sampledUtcMs), state: value.state, messages };
      view.value = { ...view.value, detailStatus: value.state, snapshot: value.state === 'ready' ? snapshot : undefined };
    } else { fail(kind); return; }
    tick();
  }
  return { view, context, visibility, select, retry, tick, receive };
}
