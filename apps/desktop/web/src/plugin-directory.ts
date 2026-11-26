import { shallowRef } from 'vue';
import { managementNumber as pluginNumber, managementRecord as record } from './management-values.ts';

export type PluginPost = (request: Readonly<Record<string, string>>) => void;
export interface PluginPackage {
  id: string; name: string; version: string; state: 'installed' | 'active' | 'stopping' | 'failed';
  installed: boolean; loaded: boolean; generation: string; canLoad: boolean; canUnload: boolean;
  loadedName?: string; loadedVersion?: string; errorCode?: number;
}
export interface PluginReading { instance: string; revision: string; packages: readonly PluginPackage[]; unavailableCount: number }
export interface PluginDirectoryView {
  status: 'loading' | 'ready' | 'stale' | 'unavailable';
  reading?: PluginReading; selected: string; notice: string;
}

// 管理计数和原生错误码只接受可完整表示的非负整数。
function count(value: unknown): value is number { return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0; }

// 投影整份包目录，重复身份或缺项会使本次读数失效。
// 步骤：校验各行必需字段 → 按包号去重 → 只保留公开属性。
function packages(raw: unknown): PluginPackage[] | undefined {
  if (!Array.isArray(raw)) return;
  const rows: PluginPackage[] = [], ids = new Set<string>();
  for (const item of raw) {
    if (!record(item) || typeof item.id !== 'string' || !item.id || ids.has(item.id) ||
        typeof item.name !== 'string' || typeof item.version !== 'string' || !pluginNumber(item.generation) ||
        typeof item.installed !== 'boolean' || typeof item.loaded !== 'boolean' ||
        typeof item.canLoad !== 'boolean' || typeof item.canUnload !== 'boolean') return;
    const state = item.state;
    if (state !== 'installed' && state !== 'active' && state !== 'stopping' && state !== 'failed') return;
    if (item.loaded !== (item.generation !== '0') || item.loaded !== (state !== 'installed') ||
        (!item.loaded && !item.installed) || (item.canLoad && (!item.installed || item.loaded)) ||
        (item.canUnload && !item.loaded)) return;
    const row: PluginPackage = { id: item.id, name: item.name, version: item.version, state,
      generation: item.generation, loaded: item.loaded, installed: item.installed, canLoad: item.canLoad, canUnload: item.canUnload };
    if (row.loaded) {
      if (typeof item.loadedName !== 'string' || typeof item.loadedVersion !== 'string' || !count(item.errorCode)) return;
      row.loadedName = item.loadedName; row.loadedVersion = item.loadedVersion; row.errorCode = item.errorCode;
    }
    ids.add(item.id);
    rows.push(row);
  }
  return rows;
}

// 每个设置窗口独立选择包，只从当前管理连接取目录。
// 步骤：绑定当窗的请求 → 核对后台与整份采样 → 保留有效选择或提示重新选择。
export function usePluginDirectory(post: PluginPost, clock = () => performance.now()) {
  const view = shallowRef<PluginDirectoryView>({ status: 'loading', selected: '', notice: '' });
  const prefix = 'pkg-' + crypto.randomUUID() + '-';
  let sequence = 0, due = 0, active = false, connected = false, discover = true;
  let pending: { request: string; instance?: string; deadline: number } | undefined;

  // 读取失败后保留最近核实的目录，安排重新发现原后台。
  function fail(): void {
    pending = undefined; discover = true; due = clock() + 1500;
    view.value = { ...view.value, status: 'unavailable' };
  }
  // 在可见且已连接的设置页最多保留一个目录请求。
  // 步骤：结算超时 → 固定请求及原后台 → 发出只读查询或标记失败。
  function tick(): void {
    if (!active || !connected) return;
    const now = clock();
    if (pending && now >= pending.deadline) fail();
    if (pending || now < due) return;
    const request = prefix + ++sequence, instance = discover ? undefined : view.value.reading?.instance;
    pending = { request, instance, deadline: now + 3500 };
    const fields: Record<string, string> = { type: 'plugins.list', version: '1', request };
    if (instance) fields.instance = instance;
    try { post(fields); } catch { fail(); }
  }
  // 切换设置或连接时废弃上次读取，原窗口保留自己的选择。
  // 步骤：取消旧请求 → 区分上次采样与当前读数 → 可见时重新发现。
  function context(online: boolean, visible: boolean): void {
    if (online === connected && visible === active) return;
    connected = online; active = visible; pending = undefined; discover = true; due = 0;
    view.value = { ...view.value, status: !online && view.value.reading ? 'stale' : online ? 'loading' : 'unavailable' };
    tick();
  }
  // 重新读取当前设置目录，已在途的同一次查询继续等候原回包。
  function refresh(): void { due = 0; tick(); }
  // 只选择当前有效目录中的明确包号。
  function select(id: string): void {
    if (view.value.status === 'ready' && view.value.reading?.packages.some(item => item.id === id))
      view.value = { ...view.value, selected: id, notice: '' };
  }
  // 用原请求、后台身份和完整行集更新设置，跨后台或丢失的包需重新选择。
  // 步骤：匹配当前请求 → 区分错误及过期读数 → 接纳整份目录并核对选中项。
  function receive(value: unknown): void {
    if (!pending || !record(value) || value.request !== pending.request) return;
    const bound = pending.instance;
    pending = undefined; due = clock() + 1500;
    if (value.version !== '1' || value.event !== 'plugins.directory' || typeof value.instance !== 'string' ||
        !value.instance || !pluginNumber(value.revision)) { fail(); return; }
    const old = view.value;
    const replaced = !!old.reading && old.reading.instance !== value.instance;
    if (value.state === 'stale') {
      discover = true;
      view.value = { ...old, status: 'stale', selected: replaced ? '' : old.selected,
        notice: replaced && old.selected ? '后台实例已更换，请重新选择插件。' : old.notice };
      return;
    }
    const rows = packages(value.packages);
    if (value.state !== 'ready' || !rows || !count(value.unavailableCount) || (bound && bound !== value.instance)) { fail(); return; }
    const lost = !!old.selected && !rows.some(row => row.id === old.selected);
    discover = false;
    view.value = { reading: { instance: value.instance, revision: value.revision, packages: rows, unavailableCount: value.unavailableCount }, status: 'ready',
      selected: replaced || lost ? '' : old.selected,
      notice: replaced && old.selected ? '后台实例已更换，请重新选择插件。' : lost ? '原先选择的插件已移出目录。' : old.notice };
  }
  return { view, select, context, receive, refresh, tick };
}
