import { shallowRef } from 'vue';
import type { PluginPost, PluginReading, usePluginDirectory } from './plugin-directory.ts';
import { managementNumber as pluginNumber, managementRecord as object } from './management-values.ts';

type Directory = ReturnType<typeof usePluginDirectory>;
type StoragePort = Pick<Storage, 'getItem' | 'setItem'>;
export type PluginAction = 'load' | 'unload';
export interface PluginWork {
  instance: string; command: string; action: PluginAction; id: string; revision: string; targetGeneration: string;
  operation: string; state: string; effect: string; generation: string;
}
const unfinished = new Set(['sending', 'queued', 'running', 'pending']);
const states = new Set(['queued', 'running', 'pending', 'succeeded', 'failed', 'busy', 'inUse', 'stale', 'conflict',
  'outOfOrder', 'expired', 'unknown', 'unavailable', 'full', 'invalid']);
const effects = new Set(['unconfirmed', 'loaded', 'unloaded', 'stopping', 'retained', 'notApplied', 'notLoaded', 'unknown']);

// 只有宿主已确认的生命周期结果才可作为成功，受理和停止中保留各自的阶段。
function validOutcome(state: string, effect: string, original: PluginWork, generation: unknown): boolean {
  if (state === 'queued' || state === 'running') return effect === 'unconfirmed';
  if (state === 'succeeded') return original.action === 'load' ? effect === 'loaded' && generation !== '0' : effect === 'unloaded';
  if (state === 'pending') return original.action === 'unload' && effect === 'stopping';
  if (state === 'unknown' || state === 'expired') return effect === 'unknown';
  return effect === 'notApplied' || effect === 'retained' || effect === 'notLoaded' || effect === 'unconfirmed';
}

// 只恢复原后台明确的命令和操作号，页面重复打开不会重写命令。
function savedWork(value: unknown, instance: string): value is PluginWork {
  return object(value) && value.instance === instance && (value.action === 'load' || value.action === 'unload') &&
    typeof value.id === 'string' && !!value.id && pluginNumber(value.command) && value.command !== '0' &&
    pluginNumber(value.revision) && pluginNumber(value.targetGeneration) && pluginNumber(value.generation) &&
    typeof value.operation === 'string' && value.operation.length <= 160 && typeof value.state === 'string' &&
    (states.has(value.state) || value.state === 'sending' || value.state === 'uncertain') &&
    typeof value.effect === 'string' && effects.has(value.effect);
}

// 对已选择的插件只提交一次明确写入，之后沿原操作读取，所有数字身份按原值传递。
// 步骤：核对当前目录与授权 → 固定用户意图 → 匹配原回执 → 刷新实效所在目录。
export function usePluginActions(directory: Directory, post: PluginPost, storage?: StoragePort, clock = () => performance.now()) {
  const work = shallowRef<PluginWork>(), readFailed = shallowRef(false);
  const prefix = 'op-' + crypto.randomUUID() + '-';
  let bound = '', nextCommand = '1', sequence = 0, due = Infinity;
  let blockedReading: PluginReading | undefined;
  let pending: { request: string; write: boolean; deadline: number } | undefined;

  // 本窗只保存原命令和公开回执，存储不可用时仍可在当前页面读取结果。
  function save(): void {
    if (work.value?.instance !== bound) return;
    try { storage?.setItem('dfy.plugin.work.' + bound, JSON.stringify({ nextCommand, work: work.value })); } catch {}
  }
  // 换后台或刷新文档时只恢复同一后台的原操作。
  // 步骤：取消旧查询 → 校验当窗的恢复记录 → 安排按原号补读。
  function bind(instance: string): void {
    if (bound === instance) return;
    bound = instance; nextCommand = '1'; pending = undefined; due = Infinity; blockedReading = undefined; readFailed.value = false;
    try {
      const saved: unknown = JSON.parse(storage?.getItem('dfy.plugin.work.' + bound) ?? 'null');
      if (!object(saved) || !pluginNumber(saved.nextCommand) || saved.nextCommand === '0' || !savedWork(saved.work, bound)) return;
      nextCommand = saved.nextCommand;
      work.value = saved.work.state === 'sending' ? { ...saved.work, state: 'uncertain' } : saved.work;
      if (work.value.operation) due = 0;
    } catch {}
  }
  // 只有新鲜目录中本窗选定的包、宿主授权和匹配代次可接受一次新操作。
  function canStart(action: PluginAction): boolean {
    const { reading, status, selected } = directory.view.value;
    if (status !== 'ready' || !reading || reading === blockedReading || !pluginNumber(nextCommand)) return false;
    if (work.value?.instance === reading.instance && (pending || unfinished.has(work.value.state) || work.value.state === 'uncertain')) return false;
    const row = reading.packages.find(item => item.id === selected);
    return !!row && (action === 'load' ? row.canLoad && row.generation === '0' : row.canUnload && row.generation !== '0');
  }
  // 提交前保存原意图，只有明确未入原生队列时才能归还本次命令号。
  // 步骤：定位已授权目标 → 保存一次性命令 → 提交原字段或登记本地拒绝。
  function start(action: PluginAction): void {
    const { reading, selected } = directory.view.value;
    if (!reading) return;
    bind(reading.instance);
    if (!canStart(action)) return;
    const row = reading.packages.find(item => item.id === selected)!;
    const command = nextCommand, request = prefix + ++sequence;
    nextCommand = (BigInt(command) + 1n).toString();
    work.value = { instance: bound, command, action, id: row.id, revision: reading.revision, targetGeneration: row.generation,
      operation: '', state: 'sending', effect: 'unconfirmed', generation: row.generation };
    pending = { request, write: true, deadline: clock() + 3500 }; due = Infinity; readFailed.value = false;
    save();
    try { post({ type: 'plugins.' + action, version: '1', request, instance: bound, command, id: row.id, revision: reading.revision, generation: row.generation }); }
    catch { undelivered(); }
  }
  // 明确未交给原生客户端时保留拒绝事实，下一次显式意图可使用原序号。
  function undelivered(): void {
    const original = work.value;
    if (!original) return;
    nextCommand = original.command; pending = undefined; due = Infinity;
    work.value = { ...original, state: 'unavailable', effect: 'notApplied' };
    blockedReading = directory.view.value.reading;
    save(); directory.refresh();
  }
  // 区分失去提交回包与失去查询结果，已获操作号只读重试。
  function unavailable(): void {
    const original = work.value;
    if (!original) return;
    pending = undefined; readFailed.value = true;
    if (!original.operation) work.value = { ...original, state: 'uncertain', effect: 'unconfirmed' };
    due = original.operation ? clock() + 1500 : Infinity;
    save();
  }
  // 只在原后台的有效目录下轮询，每个窗口至多保留一个提交或查询请求。
  // 步骤：核对后台或超时 → 等待查询时机 → 使用宿主实际签发的操作号。
  function tick(): void {
    const { reading, status } = directory.view.value;
    if (!reading || status !== 'ready') return;
    bind(reading.instance);
    if (pending && clock() >= pending.deadline) unavailable();
    const original = work.value;
    if (pending || clock() < due || original?.instance !== bound || !original.operation) return;
    const request = prefix + ++sequence;
    pending = { request, write: false, deadline: clock() + 3500 }; due = Infinity;
    try { post({ type: 'plugins.receipt', version: '1', request, instance: bound, operation: original.operation }); }
    catch { unavailable(); }
  }
  // 用户可补读同一操作，补读按钮只触发回执查询。
  function check(): void { due = 0; tick(); }
  // 只接受原请求、原意图和原代次一致的回执，终态刷新真实目录。
  // 步骤：核对回复来源 → 区分失联与已受理 → 发布实效或沿原操作继续查询。
  function receive(value: unknown): void {
    const original = work.value;
    if (!original || !pending || !object(value) || value.request !== pending.request || value.version !== '1') return;
    if (value.event === 'management.error') {
      if (pending.write && value.dispatched === false) undelivered(); else unavailable();
      return;
    }
    if (value.event !== 'plugins.operation' || value.instance !== original.instance || typeof value.operation !== 'string' ||
        !value.operation || value.operation.length > 160 || (original.operation && value.operation !== original.operation) ||
        typeof value.state !== 'string' || !states.has(value.state) || typeof value.effect !== 'string' || !effects.has(value.effect)) return;
    const missing = !pending.write && value.state === 'unknown' && value.effect === 'unknown' && value.command === undefined;
    if (!missing && (value.command !== original.command || value.action !== original.action || value.id !== original.id ||
        value.revision !== original.revision || value.targetGeneration !== original.targetGeneration || !pluginNumber(value.generation))) return;
    if (!validOutcome(value.state, value.effect, original, value.generation)) return;
    if (value.state === 'outOfOrder' && pluginNumber(value.expectedCommand) && value.expectedCommand !== '0') nextCommand = value.expectedCommand;
    pending = undefined; readFailed.value = false;
    work.value = { ...original, operation: value.operation, state: value.state, effect: value.effect,
      generation: pluginNumber(value.generation) ? value.generation : original.generation };
    due = unfinished.has(value.state) ? clock() + 350 : Infinity;
    save();
    if (!unfinished.has(value.state)) { blockedReading = directory.view.value.reading; directory.refresh(); }
  }
  return { work, readFailed, start, canStart, receive, check, tick };
}
