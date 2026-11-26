export interface MailSource { id: string; label: string; kind?: string; generation?: string; plugin?: string }
export interface MailDirectory { instance: string; revision: string; sampledUtcMs?: number; sources: readonly MailSource[] }
export type MailReceiptField = { label: string; value: string } | { label: string; utcMs: number };
export interface MailLetter {
  id: string; category: string; state: string; summary?: string; categoryVersion?: number; direction?: 'toHost' | 'toPlugin';
  receipts: readonly MailReceiptField[];
}
export interface MailSnapshot {
  instance: string; source: string; revision: string; sampledUtcMs?: number; state: 'ready' | 'stale'; messages: readonly MailLetter[];
  historyLimited?: boolean;
}
export type ReadStatus = 'loading' | 'ready' | 'unavailable' | 'stale';
export interface MailboxView {
  directory?: MailDirectory; directoryStatus: ReadStatus;
  source: string; sourceLabel: string; snapshot?: MailSnapshot; detailStatus: ReadStatus; selectionNotice?: string;
}
