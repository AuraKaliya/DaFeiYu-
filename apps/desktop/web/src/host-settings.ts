import { shallowRef } from 'vue';
import { managementNumber, managementRecord } from './management-values.ts';

export interface HostConfiguration {
  instance: string; revision: string; appliedRevision: string; pendingRestart: boolean; canSave: boolean;
  provider: string; appliedProvider: string; timeout: number; appliedTimeout: number; source: 'startup' | 'user';
  min: number; max: number;
}
type Status = 'loading' | 'ready' | 'stale' | 'unavailable';
type ReadPost = (entry: Record<string, string>) => void;

// 只从实际登记的供应商和超时字段构造显示值，保存与采用使用各自的修订。
// 步骤：校验快照身份和权限 → 定位唯一已知字段 → 核验原值、类型及范围。
function configuration(value: Record<string, unknown>, instance: string): HostConfiguration | undefined {
  if (value.instance !== instance || !managementNumber(value.revision) || !managementNumber(value.appliedRevision) ||
      typeof value.pendingRestart !== 'boolean' || typeof value.canSave !== 'boolean' || !Array.isArray(value.fields)) return;
  const rows = value.fields;
  if (!rows.every(managementRecord)) return;
  const providers = rows.filter(f => f.id === 'provider'), timeouts = rows.filter(f => f.id === 'timeout_ms');
  if (providers.length !== 1 || timeouts.length !== 1) return;
  const p = providers[0], t = timeouts[0];
  if (p.type !== 'string' || p.access_mode !== 'read_only' || p.source !== 'startup' || typeof p.value !== 'string' ||
      !p.value || typeof p.appliedValue !== 'string' || !p.appliedValue || t.type !== 'integer' || t.access_mode !== 'writable' ||
      (t.source !== 'startup' && t.source !== 'user') || t.min !== 1 || t.max !== 600000) return;
  const inRange = (n: unknown): n is number => typeof n === 'number' && Number.isInteger(n) && n >= 1 && n <= 600000;
  if (!inRange(t.value) || !inRange(t.appliedValue)) return;
  return { instance, revision: value.revision, appliedRevision: value.appliedRevision, pendingRestart: value.pendingRestart,
    canSave: value.canSave, provider: p.value, appliedProvider: p.appliedValue, timeout: t.value, appliedTimeout: t.appliedValue,
    min: t.min, max: t.max, source: t.source };
}

// 只在活动设置页读取已握手后台的主体配置，快照由本窗请求身份匹配。
// 步骤：绑定当前后台 → 发起单个有界读取 → 分别发布当前、过期和不可用读数。
export function useHostSettings(post: ReadPost, clock = () => performance.now()) {
  const view = shallowRef<{ status: Status; reading?: HostConfiguration }>({ status: 'loading' });
  const prefix = 'cfg-' + crypto.randomUUID() + '-';
  let instance = '', active = false, due = 0, serial = 0;
  let pending: { request: string; deadline: number } | undefined;
  // 同一后台短时不可读时保留已核实值，仅允许补读。
  function fail(): void {
    pending = undefined; due = clock() + 1800; view.value = { ...view.value, status: 'unavailable' };
  }
  // 周期性读取活动后台，同一时刻至多有一份主体快照在途。
  // 步骤：结算已超时请求 → 绑定原实例 → 发出正式只读信封。
  function tick(): void {
    if (!active || !instance) return;
    const now = clock();
    if (pending && now >= pending.deadline) fail();
    if (pending || now < due) return;
    const request = prefix + ++serial;
    pending = { request, deadline: now + 3500 };
    try { post({ type: 'settings.read', version: '1', request, owner: 'host', instance }); } catch { fail(); }
  }
  // 切换窗口可见性或后台时使旧请求失效，旧后台的配置不出现在新后台下。
  function context(source: string, visible: boolean): void {
    if (instance === source && active === visible) return;
    const reading = view.value.reading;
    const retained = reading && (!source || reading.instance === source) ? reading : undefined;
    instance = source; active = visible; pending = undefined; due = 0;
    view.value = { reading: retained, status: active && source ? 'loading' : retained ? 'stale' : 'unavailable' };
    tick();
  }
  // 用户刷新只推进当前只读请求。
  function refresh(): void { due = 0; tick(); }
  // 用原请求与后台归属接受原生快照，失效回复不能填入当前字段。
  function receive(value: unknown): void {
    if (!pending || !managementRecord(value) || value.request !== pending.request) return;
    pending = undefined; due = clock() + 2000;
    if (value.version !== '1' || value.event !== 'settings.snapshot' || value.owner !== 'host') { fail(); return; }
    if (value.state === 'stale') { view.value = { ...view.value, status: 'stale' }; return; }
    const reading = value.state === 'ready' ? configuration(value, instance) : undefined;
    if (!reading) { fail(); return; }
    view.value = { status: 'ready', reading };
  }
  return { view, tick, context, refresh, receive };
}
