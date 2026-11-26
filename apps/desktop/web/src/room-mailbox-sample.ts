import type { MailboxView, MailDirectory, MailLetter } from './mailbox-view';

const directory: MailDirectory = { instance: 'room-preview', revision: '7', sources: [
  { id: 'room', label: '本体书房', kind: 'builtin', generation: '1' }, { id: 'empty', label: '空信箱样本', kind: 'plugin', generation: '2', plugin: 'sample' }, { id: 'unavailable', label: '读取失败样本' },
  { id: 'retired', label: '来源失效样本' },
] };
const letters: readonly MailLetter[] = [
  { id: '21', category: '房间来信', state: 'pending', summary: '书桌旁有一封新来信。', receipts: [] },
  { id: '22', category: '房间来信', state: 'received', receipts: [
    { label: '收件回执', value: '已由系统收取' }, { label: '入箱时间', utcMs: 1732660800000 }, { label: '取件时间', utcMs: 1732660802000 },
  ] },
];

// 预览只映射明确标注的本地样本，读同一封信不会改变其状态。
export function sampleMailbox(source: string, recovered: boolean): MailboxView {
  const sourceLabel = directory.sources.find(item => item.id === source)?.label ?? '';
  const detailStatus = source === 'retired' ? 'stale' : source === 'unavailable' && !recovered ? 'unavailable' : 'ready';
  return { directory, directoryStatus: 'ready', source, sourceLabel, detailStatus,
    snapshot: source && detailStatus === 'ready' ? { instance: directory.instance, source, revision: '4', state: 'ready',
      messages: source === 'room' ? letters : [] } : undefined };
}
