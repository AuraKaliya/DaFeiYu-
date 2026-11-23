import type { Round } from './rounds.ts';
import type { LineRef } from './trace-file.ts';

const phases: Readonly<Record<string, string>> = {
  'trace.open': 'Trace 开始', 'trace.closed': 'Trace 关闭', 'subagent.accepted': '宿主接纳',
  'model.prepared': '装配', 'model.submitted': '提交', 'model.exited': '模型响应', 'model.rewritten': '宿主改写',
  'tool.candidate': '工具候选', 'tool.admitted': '工具接纳', 'tool.exited': '工具退出',
  'run.exited': '回合退出', 'mail.received': '信箱读回', 'mail.accepted': '信箱接纳',
  'event.captured': '事件采样', 'event.decoded': '事件解码', 'event.dispatched': '事件分发',
  'ask.opened': '追问打开', 'ask.transition': '追问状态变化', 'ask.presented': '追问呈现',
  'interaction.delivered': '投递回执', 'interaction.acknowledged': '投递确认',
};

const statuses: Readonly<Record<string, string>> = {
  succeeded: '成功', success: '成功', completed: '完成', failed: '失败', cancelled: '取消', canceled: '取消',
  partial: '部分完成', sent: '已发言', accepted: '已接纳', rejected: '已拒绝', blocked: '受阻',
  unknown: '效果未知', invalid_plan: '计划无效', running: '工作已开始', stopped: '已停止',
  prepared: '已装配', submitted: '已提交', answered: '已回答', expired: '已过期', finished: '已结束',
};

// 为已知宿主事件提供短标签，其他代码保留原值。
export function phaseLabel(code: string) {
  return Object.hasOwn(phases, code) ? phases[code] : code;
}

// 仅翻译已有状态，缺失字段由所在视图说明。
export function statusLabel(code: string) {
  return Object.hasOwn(statuses, code) ? statuses[code] : code;
}

// 选择状态色，文字仍显示原有的业务状态。
export function tone(code?: string) {
  if (code && ['succeeded', 'success', 'completed', 'accepted', 'sent', 'answered'].includes(code)) return 'success';
  if (code && ['failed', 'rejected', 'invalid_plan', 'cancelled', 'canceled'].includes(code)) return 'error';
  if (code && ['partial', 'blocked', 'unknown', 'pending'].includes(code)) return 'pending';
  return 'neutral';
}

// 用接纳和退出记录解释页状态；没有记录的状态保留待确认。
export function roundStatus(round: Round) {
  if (!round.accepted) return '接纳待确认';
  if (!round.exit) return '未见退出';
  return round.exit.result ? statusLabel(round.exit.result) : '已退出';
}

// 把宿主纳秒差值转换为合适的人读表记。
export function duration(value?: number) {
  if (value === undefined) return '—';
  if (value >= 1000) return `${(value / 1000).toFixed(value >= 10000 ? 1 : 2)} s`;
  if (value > 0 && value < 0.1) return '<0.1 ms';
  return `${value.toFixed(value >= 10 || value === 0 ? 0 : 1)} ms`;
}

// 同时显示来源文件和原始行号。
export function sourceLabel(ref: LineRef) {
  return `${ref.stream === 'game' ? 'game-actions.jsonl' : 'events.jsonl'} · 第 ${ref.line} 行`;
}
