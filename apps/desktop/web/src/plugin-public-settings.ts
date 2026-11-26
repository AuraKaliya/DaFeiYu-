import { managementNumber, managementRecord } from './management-values.ts';

export interface PluginPublicTarget { instance: string; id: string; generation: string }
export interface PublicField { id: string; value: string; source: 'default' | 'user' }
type FieldStatus = 'loading' | 'ready' | 'notDeclared' | 'stale' | 'unavailable';
export interface PluginPublicView { status: FieldStatus; fields: PublicField[] }

// 只有来自特定后台、插件和原加载代次的正式只读字段可以显示。
// 步骤：匹配原请求及包 → 区分过期、未声明和读取失败 → 整批核对已采用字符串及来源。
export function readPluginFields(raw: unknown, request: string, target: PluginPublicTarget): PluginPublicView | undefined {
  if (!managementRecord(raw) || raw.request !== request) return;
  const unavailable: PluginPublicView = { status:'unavailable', fields:[] };
  if (raw.version !== '1' || raw.event !== 'settings.snapshot' || raw.owner !== 'plugin' || raw.id !== target.id ||
      raw.generation !== target.generation || raw.canSave !== false || !Array.isArray(raw.fields) ||
      typeof raw.instance !== 'string' || !raw.instance) return unavailable;
  if (raw.state === 'stale' && !raw.fields.length) return { status:'stale', fields:[] };
  if (raw.instance !== target.instance || !managementNumber(raw.revision) || raw.revision !== target.generation ||
      raw.appliedRevision !== target.generation) return unavailable;
  if (raw.state === 'notDeclared' && !raw.fields.length) return { status:'notDeclared', fields:[] };
  if (raw.state !== 'ready' || raw.fields.length < 1 || raw.fields.length > 32) return unavailable;
  const fields: PublicField[] = [], ids = new Set<string>();
  for (const f of raw.fields) {
    if (!managementRecord(f) || typeof f.id !== 'string' || !/^[A-Za-z][A-Za-z0-9_.-]{0,63}$/.test(f.id) || ids.has(f.id) ||
        f.type !== 'string' || f.access_mode !== 'read_only' || (f.source !== 'default' && f.source !== 'user') ||
        typeof f.value !== 'string' || f.appliedValue !== f.value || f.value.length > 4096 || new TextEncoder().encode(f.value).length > 4096) return unavailable;
    ids.add(f.id); fields.push({ id:f.id, value:f.value, source:f.source });
  }
  return { status:'ready', fields };
}
