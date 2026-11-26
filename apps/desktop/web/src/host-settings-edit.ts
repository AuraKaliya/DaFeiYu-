import { shallowRef } from 'vue';
import { managementNumber, managementRecord } from './management-values.ts';
import type { useHostSettings } from './host-settings.ts';

type Settings = ReturnType<typeof useHostSettings>;
type Intent = { instance: string; revision: string; submission: string; timeout: number; state: string; savedRevision?: string };
type SaveRequest = Record<string, string | { timeout_ms: number }>;
const finalStates = new Set(['saved','conflict','submissionConflict','readOnly','invalid','failed','storageChanged','stale','unavailable','unknown']);

// 保存身份只携带本窗提交的有效超时和原基准，跨文档恢复走只读回执。
function validIntent(value: unknown): value is Intent {
  return managementRecord(value) && typeof value.instance === 'string' && !!value.instance && managementNumber(value.revision) &&
    typeof value.submission === 'string' && /^[\w./-]{1,96}$/.test(value.submission) && typeof value.timeout === 'number' &&
    Number.isInteger(value.timeout) && value.timeout >= 1 && value.timeout <= 600000 &&
    typeof value.state === 'string' && (finalStates.has(value.state) || value.state === 'sending' || value.state === 'uncertain') &&
    (value.savedRevision === undefined || managementNumber(value.savedRevision));
}

// 在草稿最初编辑的修订上保存，冲突后由用户决定是否沿用输入。
// 步骤：固定输入来源和修订 → 明确提交或按原号补读 → 保存回执与实际生效值分别显示。
export function useHostSettingEdit(settings: Settings, post: (fields: SaveRequest) => void,
    storage?: Pick<Storage,'getItem'|'setItem'>, clock = () => performance.now()) {
  const draft = shallowRef<{ instance: string; revision: string; text: string }>();
  const receipt = shallowRef<Intent>(), lookupNotice = shallowRef('');
  const prefix = 'save-' + crypto.randomUUID() + '-';
  let instance = '', serial = 0, due = Infinity;
  let pending: { instance: string; request: string; write: boolean; deadline: number } | undefined;
  // 捕获本窗原提交，以便刷新后继续查证。
  function remember(): boolean {
    if (!storage || !receipt.value) return false;
    try { storage.setItem('dfy.settings.submission',JSON.stringify(receipt.value)); return true; } catch { return false; }
  }
  // 连接首次就绪或后台更换时恢复原提交，只允许在新后台按稳定提交号补读。
  // 步骤：释放旧查询 → 校验保存意图 → 安排原提交补读。
  function bind(source: string): void {
    if (source === instance) return;
    instance = source; draft.value = undefined; pending = undefined; due = Infinity; lookupNotice.value = '';
    if (!receipt.value) {
      try {
        const saved: unknown = JSON.parse(storage?.getItem('dfy.settings.submission') ?? 'null');
        if (validIntent(saved)) receipt.value = saved.state === 'sending' ? { ...saved, state: 'uncertain' } : saved;
      } catch {}
    }
    if (receipt.value && (receipt.value.state === 'sending' || receipt.value.state === 'uncertain' || receipt.value.state === 'saved')) due = 0;
  }
  // 按首次输入的后台和修订保留草稿，后续轮询不改写编辑内容。
  function input(text: string): void {
    const { reading, status } = settings.view.value;
    if (!reading?.canSave || status !== 'ready' || busy()) return;
    bind(reading.instance);
    draft.value = { instance: reading.instance, revision: draft.value?.revision ?? reading.revision, text };
  }
  // 只接受协议范围内完整的整数输入。
  function value(): number | undefined {
    const text = draft.value?.text, reading = settings.view.value.reading;
    if (!text || !reading || !/^\d+$/.test(text)) return;
    const parsed = Number(text);
    if (Number.isSafeInteger(parsed) && parsed >= reading.min && parsed <= reading.max) return parsed;
  }
  // 当原保存仍在途或结果待确认时等待原提交回执。
  function busy(): boolean { return receipt.value?.state === 'sending' || receipt.value?.state === 'uncertain'; }
  // 只允许保存已核实权限、来源与基准相同且有实际改动的草稿。
  function canSave(): boolean {
    const { reading, status } = settings.view.value, n = value(), entry = draft.value;
    return !!reading?.canSave && status === 'ready' && !!entry && entry.instance === reading.instance &&
      entry.revision === reading.revision && n !== undefined && n !== reading.timeout && !busy() && !pending;
  }
  // 用户确认已看到新值后，才将保留的草稿改绑到当前修订。
  function rebase(): void {
    const { reading, status } = settings.view.value, entry = draft.value;
    if (reading?.canSave && status === 'ready' && entry?.instance === reading.instance && !busy())
      draft.value = { ...entry, revision: reading.revision };
  }
  // 用户选择当前值时清除尚未提交的本窗草稿。
  function reset(): void { if (!busy()) draft.value = undefined; }
  // 仅点保存时产生新提交号，传输不确定转为按原号查询。
  // 步骤：复核修订与输入 → 先存原提交 → 发送同一意图并等待回执。
  function save(): void {
    const entry = draft.value, timeout = value();
    if (!entry || timeout === undefined || !canSave()) return;
    const submission = crypto.randomUUID(), request = prefix + ++serial;
    receipt.value = { instance: entry.instance, revision: entry.revision, submission, timeout, state: 'sending' };
    pending = { instance: entry.instance, request, write: true, deadline: clock() + 3500 }; due = Infinity; lookupNotice.value = '';
    if (!remember()) {
      pending = undefined; receipt.value = { ...receipt.value, state:'unavailable' };
      lookupNotice.value = '当前窗口无法保存提交号，本次保存未提交。';
      return;
    }
    try { post({ type: 'settings.save', version: '1', owner: 'host', request, instance: entry.instance,
      revision: entry.revision, submission, changes: { timeout_ms: timeout } }); }
    catch { failed(true); }
  }
  // 明确未分派可重新编辑，写入结果未知时沿原稳定号核实。
  // 步骤：结束当前请求 → 保留已核实保存或记录未确认 → 安排所需补读。
  function failed(unsent: boolean): void {
    if (!receipt.value) return;
    pending = undefined;
    if (receipt.value.state === 'saved') {
      lookupNotice.value = '暂时无法复查原提交，保留上次核实的保存结果。'; due = Infinity; return;
    }
    receipt.value = { ...receipt.value, state: unsent ? 'unavailable' : 'uncertain' };
    due = unsent ? Infinity : clock() + 1200; remember();
  }
  // 使用当前已握手实例查询本份用户配置的原提交，不重放保存。
  // 步骤：核对当前绑定 → 结算传输超时 → 以原提交号读取已保存实效。
  function tick(): void {
    const { reading, status } = settings.view.value;
    if (!reading || status !== 'ready') return;
    bind(reading.instance);
    if (pending && clock() >= pending.deadline) failed(false);
    if (pending || !receipt.value || clock() < due) return;
    const request = prefix + ++serial;
    pending = { instance, request, write: false, deadline: clock() + 3500 }; due = Infinity;
    try { post({ type: 'settings.receipt', version:'1', owner:'host', request, instance, submission:receipt.value.submission }); }
    catch { failed(false); }
  }
  // 从当前配置后台按原提交补读，不创建新的写命令。
  function check(): void { due = 0; tick(); }
  // 核对原提交及其实际保存值，当前运行值随后从独立快照读取。
  // 步骤：匹配本窗请求 → 验证保存实效或拒绝原因 → 保留冲突草稿并刷新当前配置。
  function receive(raw: unknown): void {
    const original = receipt.value;
    if (!original || !pending || !managementRecord(raw) || raw.request !== pending.request || raw.version !== '1') return;
    if (raw.event === 'management.error') { failed(pending.write && raw.dispatched === false); return; }
    if (raw.event !== 'settings.result' || raw.instance !== pending.instance || raw.owner !== 'host' || raw.submission !== original.submission ||
        typeof raw.state !== 'string' || !finalStates.has(raw.state) || !managementNumber(raw.revision) ||
        !managementNumber(raw.appliedRevision) || typeof raw.pendingRestart !== 'boolean') return;
    if (raw.state === 'saved' && (!managementNumber(raw.savedRevision) || raw.savedValue !== original.timeout ||
        BigInt(raw.savedRevision) !== BigInt(original.revision) + 1n)) return;
    pending = undefined; due = Infinity;
    if (original.state === 'saved' && raw.state !== 'saved') {
      lookupNotice.value = '当前后台未能提供这份原回执，保留上次核实的保存结果。';
      settings.refresh(); return;
    }
    lookupNotice.value = '';
    receipt.value = { ...original, state:raw.state, savedRevision:raw.state === 'saved' ? raw.savedRevision as string : undefined };
    if (raw.state === 'saved' && draft.value?.instance === original.instance && draft.value.revision === original.revision &&
        value() === original.timeout) draft.value = undefined;
    remember(); settings.refresh();
  }
  return { draft, receipt, lookupNotice, input, value, busy, canSave, rebase, reset, save, tick, check, receive };
}
