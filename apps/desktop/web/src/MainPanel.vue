<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, shallowRef, watch } from 'vue';
import { useMessageHistory } from './message-history';
import { messageTimeline } from './message-timeline';
import { readPosition, savePosition } from './history-viewport';
import MessageTime from './MessageTime.vue';
import MessageReceipt from './MessageReceipt.vue';
import ManagementShell from './ManagementShell.vue';
import LogSettings from './LogSettings.vue';
import RoomStateView, { type StateReading } from './RoomStateView.vue';
import ResidentRoomMailbox from './ResidentRoomMailbox.vue';
import PluginSettings from './PluginSettings.vue';
import HostSettings from './HostSettings.vue';
import { originalReceipt, recoverDialogue, rememberDialogue, rememberDraft, type WireCommand } from './dialogue-recovery';

type Operation = 'pending' | 'succeeded' | 'failed' | 'cancelled';
type CommandState = 'unavailable' | 'queued' | 'accepted' | 'busy' | 'rejected' | 'conflict' | 'expired' | 'stale' | 'full';
type InputState = 'waiting' | 'submitted' | 'cancelled' | 'deadline' | 'limit' | 'failed' | 'closed';
interface TurnInput { readonly message: string; readonly text: string; readonly state: InputState; readonly modelStep: string }
interface Turn {
  readonly session: string; readonly turn: string; readonly prompt: string; readonly answer: string;
  readonly result: Operation; readonly exit: Operation; readonly queuedMessage: string; readonly acceptsInput: boolean; readonly inputs: readonly TurnInput[];
}
interface QueuedMessage { readonly session: string; readonly message: string; readonly text: string }
interface Receipt { readonly command: string; readonly state: CommandState }
type Target = Pick<Turn, 'session' | 'turn'>;
type Pending = { readonly id: string; readonly action: 'send' | 'enqueue' | 'append'; readonly text: string } |
  { readonly id: string; readonly action: 'clear' | 'resume' | 'remove' };

const operations: ReadonlySet<unknown> = new Set<Operation>(['pending', 'succeeded', 'failed', 'cancelled']);
const commandStates: ReadonlySet<unknown> = new Set<CommandState>(['unavailable', 'queued', 'accepted', 'busy', 'rejected', 'conflict', 'expired', 'stale', 'full']);
const statusText: Record<Operation, string> = { pending: '正在回复', succeeded: '本轮已完成', failed: '本轮回复失败', cancelled: '已停止回复' };
const inputText: Record<InputState, string> = {
  waiting: '已接收，待带入本轮', submitted: '已带入本轮', cancelled: '本轮已停止，未带入', deadline: '等待超时，未带入',
  limit: '达到本轮限制，未带入', failed: '本轮失败，未带入', closed: '本轮已结束，未带入',
};
const rejection: Record<Exclude<CommandState, 'queued' | 'accepted'>, string> = {
  unavailable: '会话暂时无法使用。', busy: '会话正在处理其他操作。',
  rejected: '这次操作未被接纳。', conflict: '此命令编号已用于其他操作。', expired: '本次回执已过期。',
  stale: '这条消息的状态已改变，请查看最新对话。', full: '待发队列已满。可以先移除一条，或等它发出后再试。',
};
const byteLimit = 1024 * 1024;
const encoder = new TextEncoder();
const bridge = window.chrome.webview;
const scope = `${crypto.randomUUID()}-`;
const turns = shallowRef<readonly Turn[]>([]);
const queue = shallowRef<readonly QueuedMessage[]>([]);
const queuePaused = ref(false);
const room = shallowRef<StateReading>();
const conversation = shallowRef<StateReading>();
const roomCurrent = ref(false);
let nextRoomRead = 0, latestRoomSnapshot = 0;
let roomConfirmed = -Infinity, dialogueConfirmed = -Infinity;
const freshnessMs = 3500;
const pending = shallowRef<Pending>();
const pendingStop = ref('');
const stopTarget = shallowRef<Target>();
const transcript = ref<HTMLOListElement>();
const messages = useMessageHistory();
const timeline = computed(() => messageTimeline(messages.rows.value, turns.value, messages.instance.value, messages.more.value));
const composer = ref<HTMLTextAreaElement>();
const clearDialog = ref<HTMLDialogElement>();
const clearTarget = shallowRef<Target>();
const draft = ref('');
const destination = ref<'current' | 'next'>('current');
const notice = ref('');
const connectionNotice = ref('');
const shutdownDialog = ref<HTMLDialogElement>();
const shutdown = ref<'idle' | 'requested' | 'stopping' | 'unknown' | 'exited'>('idle');
const lastWait = ref('');
const longWait = ref(false);
let firstStopObservation = 0, nextStopRead = 0;
let shutdownRequest = '';
const ready = ref(false);
const accepting = ref(false);
const tooLong = computed(() => encoder.encode(draft.value).length > byteLimit);
const lastTurn = computed(() => turns.value.at(-1));
const queueMode = computed(() => lastTurn.value?.exit === 'pending');
const canChoose = computed(() => queueMode.value && lastTurn.value?.result === 'pending' && lastTurn.value.acceptsInput);
const appendMode = computed(() => canChoose.value && destination.value === 'current');
const canSend = computed(() => ready.value && messages.subscribed.value && (accepting.value || queueMode.value) &&
  !pending.value && !!draft.value.trim() && !tooLong.value);
const stoppedCurrent = computed(() => !!stopTarget.value && stopTarget.value.session === lastTurn.value?.session && stopTarget.value.turn === lastTurn.value?.turn);
const canStop = computed(() => ready.value && !pendingStop.value && !stoppedCurrent.value && lastTurn.value?.result === 'pending');
const canClear = computed(() => accepting.value && !pending.value && !!lastTurn.value);
// 区分页面提交、后端收尾与可接纳下一轮，上一轮的结果留在原条目。
const panelStatus = computed(() => {
  if (shutdown.value === 'exited') return '本体意外退出';
  if (shutdown.value === 'stopping') return '本体正在收尾';
  if (shutdown.value === 'requested') return '正在请求退出';
  if (connectionNotice.value) return '本体连接中断';
  if (!ready.value) return conversation.value ? '连接中断' : '正在连接';
  if (!messages.subscribed.value) return '正在连接聊天记录';
  if (lastTurn.value?.result === 'cancelled' && lastTurn.value.exit === 'pending') return '正在收尾';
  if (stoppedCurrent.value && lastTurn.value?.result === 'pending') return '已请求停止';
  if (pending.value) return { send: '正在提交', enqueue: '正在排队', append: '正在提交补充', clear: '正在重置上下文', resume: '正在恢复', remove: '正在移除' }[pending.value.action];
  if (accepting.value) return '可以聊天';
  return '正在处理对话';
});
let command = 0n;
let request = 0, latestSnapshot = 0;
let revision = '';
let refresh = 0;
const outstanding = new Map<string, WireCommand>();
const reconnecting = new Set<string>();
let originalConversation = '', nextLookup = 0;

// 在提交、结算及离开页面时固定草稿和未决原件。
function remember(): void {
  try { rememberDialogue(draft.value, outstanding.values(), originalConversation); }
  catch { notice.value = '当前窗口无法保留刷新前的草稿，请先等待操作回执。'; }
}

// 从同一窗口历史项恢复未决身份，等原生快照核对后决定是否补投原命令。
// 步骤：还原草稿 → 固定命令水位及两个槽位 → 等待正式回执。
function recover(): void {
  const saved = recoverDialogue();
  draft.value = saved.draft;
  originalConversation = saved.conversation;
  for (const fields of saved.commands) {
    const id = fields.command;
    outstanding.set(id, fields);
    reconnecting.add(id);
    if (BigInt(id) > command) command = BigInt(id);
    if (fields.type === 'dialogue.cancel') {
      pendingStop.value = id;
      stopTarget.value = { session: fields.session, turn: fields.turn };
    } else if ('text' in fields) {
      const action = fields.type === 'dialogue.submit' ? 'send' : fields.type === 'dialogue.append' ? 'append' : 'enqueue';
      pending.value = { id, action, text: fields.text };
    } else pending.value = { id, action: fields.type === 'dialogue.clear' ? 'clear' : fields.type === 'dialogue.resume' ? 'resume' : 'remove' };
  }
}

// 先确认浏览器回包是可读取字段的对象。
function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

// 验证由原生端签发的十进制身份，没有转换为有精度限制的 JS 整数。
function isDecimal(value: unknown): value is string {
  return typeof value === 'string' && /^\d{1,20}$/.test(value);
}

// 核对补充身份和实际送模步骤，未带入的原话同样保留。
function isTurnInput(value: unknown): value is TurnInput {
  return isRecord(value) && isDecimal(value.message) && typeof value.text === 'string' && typeof value.state === 'string' &&
    Object.hasOwn(inputText, value.state) && isDecimal(value.modelStep);
}

// 只接受带原会话身份、正文、补充回执及两项运行状态的显示条目。
function isTurn(value: unknown): value is Turn {
  return isRecord(value) && isDecimal(value.session) && isDecimal(value.turn) &&
    isDecimal(value.queuedMessage) && typeof value.prompt === 'string' && typeof value.answer === 'string' &&
    typeof value.acceptsInput === 'boolean' && Array.isArray(value.inputs) && value.inputs.every(isTurnInput) &&
    operations.has(value.result) && operations.has(value.exit);
}

// 只有带原生身份和完整原话的待发项可以进入列表。
function isQueuedMessage(value: unknown): value is QueuedMessage {
  return isRecord(value) && isDecimal(value.session) && isDecimal(value.message) && typeof value.text === 'string';
}

// 验证窗口命令回执，以原命令号关联本地草稿。
function isReceipt(value: unknown): value is Receipt {
  return isRecord(value) && isDecimal(value.command) && commandStates.has(value.state);
}

// 给当前页面的所有命令使用独立请求身份，便于拒绝其他文档的回包。
function post(fields: WireCommand | { type: 'dialogue.read'; revision: string; command: string } | { type: 'room.read' }): void {
  if ('command' in fields && fields.type !== 'dialogue.read') {
    outstanding.set(fields.command, fields);
    remember();
  }
  bridge.postMessage({ ...fields, version: '1', request: `${scope}${++request}` });
}

// 查询当前实例的快照与原命令回执，收尾时只观察已绑定的后台。
// 步骤：按退出状态分流 → 检查快照时效 → 查询对话及房间。
function read(): void {
  const now = performance.now();
  if (shutdown.value === 'exited') return;
  if (shutdown.value === 'stopping') {
    longWait.value = now - firstStopObservation >= 10000;
    if (now >= nextStopRead) {
      nextStopRead = now + 1000;
      post({ type: 'dialogue.read', revision, command: '' });
    }
    return;
  }
  if (now - dialogueConfirmed > freshnessMs) {
    if (ready.value) for (const id of outstanding.keys()) reconnecting.add(id);
    ready.value = false; accepting.value = false; messages.disconnect();
  }
  if (now - roomConfirmed > freshnessMs) roomCurrent.value = false;
  post({ type: 'dialogue.read', revision, command: pending.value?.id ?? pendingStop.value });
  if (pending.value && pendingStop.value) post({ type: 'dialogue.read', revision, command: pendingStop.value });
  if (now >= nextRoomRead) {
    nextRoomRead = now + 1000;
    post({ type: 'room.read' });
  }
}

// 验证房间位置与原生身份，将有序快照投影为只读状态字段。
// 步骤：核对请求顺序与载荷 → 区分断开及旧修订 → 保存已核实的当前位置和行为。
function receiveRoom(data: unknown, sequence: number): void {
  if (sequence <= latestRoomSnapshot) return;
  if (data === null) {
    latestRoomSnapshot = sequence;
    roomConfirmed = -Infinity;
    roomCurrent.value = false;
    return;
  }
  if (!isRecord(data) || !isDecimal(data.instance) || !isDecimal(data.generation) || !isDecimal(data.body) ||
      !isDecimal(data.revision) || !isDecimal(data.layoutVersion) || typeof data.roomName !== 'string' ||
      typeof data.placeName !== 'string' || !Number.isSafeInteger(data.x) || !Number.isSafeInteger(data.y) ||
      typeof data.activity !== 'string') return;

  const identity = `${data.instance}/${data.generation}/${data.body}`;
  if (room.value?.identity === identity && BigInt(data.revision) < BigInt(room.value.revision)) return;
  latestRoomSnapshot = sequence;
  roomConfirmed = performance.now();
  roomCurrent.value = true;
  room.value = { source: '本体书房', identity, revision: data.revision, time: Date.now(),
    binding: { instance: data.instance, body: data.body }, fields: {
      '当前位置': `${data.roomName} · ${data.placeName}`, '身体坐标': `(${data.x}, ${data.y})`,
      '身体行为': data.activity === 'resting' ? '停留' : '未知', '房间实例 / 代次': `${data.instance} / ${data.generation}`,
      '身体编号': data.body, '布局版本': data.layoutVersion,
    } };
}

// 从同一份原生对话快照提取状态，分别记录业务结果、实际退出与待发项。
function publishConversation(canAccept: boolean, currentRevision: string): void {
  const turn = turns.value.at(-1);
  const result = { pending: '处理中', succeeded: '成功', failed: '失败', cancelled: '已取消' };
  const exit = { pending: '尚未退出', succeeded: '已退出', failed: '退出失败', cancelled: '已取消' };
  conversation.value = { source: '主会话', identity: 'main', revision: currentRevision, time: Date.now(), fields: {
    '接纳新对话': canAccept ? '可以接纳' : '暂不可接纳', '最近回合': turn ? `${turn.session} / ${turn.turn}` : '尚无回合',
    '回合结果': turn ? result[turn.result] : '无', '实际退出': turn ? exit[turn.exit] : '无',
    '待发消息': `${queue.value.length} 条`, '队列状态': queuePaused.value ? '已暂停' : queue.value.length ? '等待发送' : '空',
  } };
}

// 只在原发送获接纳时清掉未改动的原稿，控制命令由正式快照呈现结果。
// 步骤：核对待交付身份 → 按命令结算草稿与焦点 → 显示宿主拒绝原因。
function settle(receipt: Receipt): void {
  if (receipt.state === 'queued') return;
  if (!outstanding.delete(receipt.command)) return;
  reconnecting.delete(receipt.command);
  if (receipt.command === pendingStop.value) {
    pendingStop.value = '';
    if (receipt.state !== 'accepted') {
      stopTarget.value = undefined;
      notice.value = rejection[receipt.state];
    }
    remember();
    return;
  }
  const current = pending.value;
  if (!current || current.id !== receipt.command) return;
  pending.value = undefined;
  if (receipt.state === 'accepted') {
    if ('text' in current && draft.value === current.text) draft.value = '';
    if (current.action === 'clear') composer.value?.focus();
  } else if (current.action === 'append' && receipt.state === 'full') notice.value = '本轮可接收的补充已满，草稿已保留。';
  else if (current.action === 'append' && receipt.state === 'stale') notice.value = '原回合已结束，草稿已保留，可以另发。';
  else notice.value = rejection[receipt.state];
  remember();
}

// 恢复时先查同一逻辑会话的持久回执，只有确认无记录才补投原件。
// 步骤：核对连接与提交身份 → 查询原记录 → 结算草稿或沿原号重投。
function recoverOutstanding(latestCommand: bigint): void {
  for (const [id, fields] of outstanding) {
    if (!reconnecting.has(id)) continue;
    if (!('submission' in fields) || !fields.submission || !('text' in fields)) {
      reconnecting.delete(id);
      if (BigInt(id) > latestCommand) post(fields);
      continue;
    }
    if (!messages.subscribed.value || performance.now() < nextLookup) continue;
    if (originalConversation !== messages.conversation.value) { settle({ command: id, state: 'stale' }); continue; }
    nextLookup = performance.now() + 600;
    messages.receipt('desktop', fields.submission, row => {
      if (row === undefined || outstanding.get(id) !== fields || !reconnecting.has(id)) return;
      if (!row) {
        reconnecting.delete(id);
        post(fields);
        return;
      }
      const state = originalReceipt(fields, row);
      if (state === 'pending') return;
      settle({ command: id, state });
      if (state === 'unavailable' && row.receipt.state === 'unknown') notice.value = '投递结果待核对，草稿已保留。';
    });
  }
}

// 开始观察后台收尾，同时冻结已失去时效的房间与会话状态。
function observeStop(): void {
  if (shutdown.value !== 'stopping') firstStopObservation = performance.now();
  shutdown.value = 'stopping';
  ready.value = false;
  accepting.value = false;
  roomCurrent.value = false;
  connectionNotice.value = '';
}

// 依据原生进程事实展示退出码或最近一次核实的等待对象。
// 步骤：验证内核状态 → 展示实际退出或进入收尾 → 核对阶段与原回合标识。
function receiveLifecycle(value: Record<string, unknown>): void {
  if (typeof value.requested !== 'boolean' || typeof value.pid !== 'number' || !Number.isSafeInteger(value.pid) || value.pid <= 0) return;
  const code = value.exitCode;
  if (typeof code === 'number' && Number.isSafeInteger(code) && code >= 0 && code <= 0xffffffff) {
    if (value.requested && code === 0) return;
    shutdown.value = 'exited';
    ready.value = accepting.value = roomCurrent.value = false;
    connectionNotice.value = `原后台进程 ${value.pid} 已结束，退出码 ${code}。可在设置中查看日志或关闭此窗口。`;
    return;
  }
  if (code !== null || !value.requested) return;
  observeStop();
  const progress = value.progress;
  if (!isRecord(progress)) return;
  if (progress.stage === 'conversation' && isDecimal(progress.session) && isDecimal(progress.turn) && progress.turn !== '0')
    lastWait.value = `最近确认正在等待会话 ${progress.session} 的回合 ${progress.turn} 退出。`;
  else if (progress.stage === 'conversation') lastWait.value = '最近确认正在等待会话退出。';
  else if (progress.stage === 'plugins') lastWait.value = '最近确认正在等待插件卸载。';
  else if (progress.stage === 'complete') lastWait.value = '后台工作已收尾，正在等待进程退出。';
  else if (progress.stage === 'failed') lastWait.value = '后台报告收尾失败，正在等待进程退出。';
}

// 将原生快照投影为当前页面的对话，过滤协议不符及迟到的旧快照。
// 步骤：核对页面请求 → 验证快照字段并更新正文 → 结算匹配的窗口命令。
function receive(event: MessageEvent<unknown>): void {
  const value = event.data;
  if (messages.receive(value)) return;
  if (!isRecord(value) || value.version !== '1' || typeof value.request !== 'string' || !value.request.startsWith(scope)) return;
  const sequence = Number(value.request.slice(scope.length));
  if (!Number.isSafeInteger(sequence) || sequence <= 0) return;
  if (shutdown.value === 'exited') return;
  if (value.event === 'host.lifecycle') { receiveLifecycle(value); return; }
  if (value.request === shutdownRequest && value.event === 'host.stopping' && value.state === 'stopping') {
    observeStop();
    return;
  }
  if (shutdown.value === 'stopping') return;
  if (value.event === 'management.error') {
    if (value.request === shutdownRequest) shutdown.value = 'unknown';
    if (sequence < latestSnapshot) return;
    ready.value = false;
    messages.disconnect();
    accepting.value = false;
    roomCurrent.value = false;
    connectionNotice.value = typeof value.reason === 'string' && value.reason.includes('configuration differs')
      ? '本体已使用另一份启动配置，请用相同配置重新打开管理中心。'
      : '无法连接后台本体。请检查本体状态，再重新打开管理中心。';
    for (const id of outstanding.keys()) reconnecting.add(id);
    return;
  }
  if (value.event === 'room.snapshot') {
    receiveRoom(value.room, sequence);
    return;
  }
  if (value.event === 'dialogue.snapshot') {
    if (sequence <= latestSnapshot || typeof value.ready !== 'boolean' ||
        typeof value.accepting !== 'boolean' || !isDecimal(value.latestCommand) || typeof value.revision !== 'string' ||
        typeof value.queuePaused !== 'boolean' || (value.turns === null) !== (value.queue === null) ||
        (value.turns !== null && (!Array.isArray(value.turns) || !value.turns.every(isTurn))) ||
        (value.queue !== null && (!Array.isArray(value.queue) || !value.queue.every(isQueuedMessage)))) return;
    if (value.ready && !isDecimal(value.revision)) return;
    latestSnapshot = sequence;
    connectionNotice.value = '';
    dialogueConfirmed = performance.now();
    ready.value = value.ready;
    accepting.value = value.accepting;
    queuePaused.value = value.queuePaused;
    const latestCommand = BigInt(value.latestCommand);
    if (latestCommand > command) command = latestCommand;
    if (value.turns !== null && value.queue !== null) {
      revision = value.revision;
      turns.value = value.turns;
      queue.value = value.queue;
    }
    if (value.ready) publishConversation(value.accepting, value.revision);
    if (value.ready) messages.pulse(value.messageWatermark);
    if (value.confirmStop === true) askStop();
    if (value.ready && reconnecting.size) recoverOutstanding(latestCommand);
  } else if (value.event !== 'dialogue.receipt') return;
  if (isReceipt(value.receipt) && !reconnecting.has(value.receipt.command)) settle(value.receipt);
}

// 设置按钮与托盘只打开同一确认框，重复请求保持当前焦点。
function askStop(): void {
  if (ready.value && shutdown.value === 'idle' && !shutdownDialog.value?.open) shutdownDialog.value?.showModal();
}

// 用户确认后向当前已绑定的本体提交退出，并用原请求号核对收尾回执。
function stopHost(): void {
  shutdownDialog.value?.close();
  if (!ready.value || shutdown.value !== 'idle') return;
  shutdown.value = 'requested';
  shutdownRequest = `${scope}${++request}`;
  bridge.postMessage({ type: 'host.stop', version: '1', request: shutdownRequest });
}

// 按输入区当前标明的去向发送原稿，只有原生接纳后才清除。
// 步骤：核对草稿 → 固定命令和所见回合 → 按所选去向提交普通问答、补充或下一轮。
function send(): void {
  if (!canSend.value) return;
  const id = String(++command), target = lastTurn.value;
  const text = draft.value;
  const original = { command: id, submission: crypto.randomUUID(), sourceUtcMs: Date.now(), text };
  originalConversation = messages.conversation.value;
  notice.value = '';
  if (target && queueMode.value) {
    const action = appendMode.value ? 'append' : 'enqueue';
    pending.value = { id, action, text };
    post({ type: `dialogue.${action}`, ...original, instance: messages.instance.value, session: target.session, turn: target.turn });
  } else {
    pending.value = { id, action: 'send', text };
    accepting.value = false;
    post({ type: 'dialogue.submit', ...original });
  }
}

// 由用户把未带入的原话放回编辑区，保留已有草稿并等待显式发送。
function reuseInput(input: TurnInput): void {
  const combined = draft.value ? draft.value + '\n' + input.text : input.text;
  if (encoder.encode(combined).length > byteLimit) {
    notice.value = '加入后草稿会超过 1 MiB，请先缩短当前草稿。';
    return;
  }
  draft.value = combined;
  composer.value?.focus();
}

// 只操作列表中的原消息，恢复命令由宿主再次核对队首。
function editQueue(action: 'resume' | 'remove', item: QueuedMessage): void {
  if (pending.value) return;
  const id = String(++command);
  pending.value = { id, action };
  notice.value = '';
  post({ type: `dialogue.${action}`, command: id, session: item.session, message: item.message });
}

// 使用独立控制槽位停止当前回合，正在接纳的待发项继续沿原编号核对。
function stop(): void {
  const target = lastTurn.value;
  if (!canStop.value || !target) return;
  pendingStop.value = String(++command);
  stopTarget.value = { session: target.session, turn: target.turn };
  notice.value = '';
  post({ type: 'dialogue.cancel', command: pendingStop.value, ...stopTarget.value });
}

// 打开清空确认时固定所见回合，避免确认期间的新对话被清掉。
function askClear(): void {
  if (!canClear.value || !lastTurn.value) return;
  clearTarget.value = { session: lastTurn.value.session, turn: lastTurn.value.turn };
  clearDialog.value?.showModal();
}

// 确认后按原回合请求清空，界面只根据宿主读回收起历史。
function confirmClear(): void {
  const target = clearTarget.value;
  clearDialog.value?.close();
  if (!target || !canClear.value || lastTurn.value?.session !== target.session || lastTurn.value.turn !== target.turn) {
    notice.value = '对话已更新，请重新选择要重置的上下文。';
    return;
  }
  const id = String(++command);
  pending.value = { id, action: 'clear' };
  notice.value = '';
  post({ type: 'dialogue.clear', command: id, ...target });
}

// 区分正常发送、输入法确认和 Shift+Enter 换行。
function enter(event: KeyboardEvent): void {
  if (event.isComposing || event.keyCode === 229 || event.shiftKey) return;
  event.preventDefault();
  send();
}

// 新消息只在读者位于末尾时自动跟随，补入旧页后固定屏幕中的原条目。
// 步骤：更新前定位可见条目 → 等 Vue 完成排版 → 按原位置或列表末尾滚动。
watch(timeline, async () => {
  const list = transcript.value;
  if (!list || !list.clientHeight) return;
  const follow = list.scrollHeight - list.clientHeight - list.scrollTop < 24 && !readingEarlier;
  const top = list.getBoundingClientRect().top;
  const anchor = Array.from(list.querySelectorAll<HTMLElement>('[data-entry]')).find(el => el.getBoundingClientRect().bottom > top);
  const offset = anchor?.getBoundingClientRect().top;
  await nextTick();
  if (restorePosition && messages.conversation.value) {
    if (restorePosition.conversation !== messages.conversation.value) restorePosition = undefined;
    else {
      const original = list.querySelector<HTMLElement>(`[data-stored-message="${restorePosition.message}"]`);
      if (original) {
        list.scrollTop += original.getBoundingClientRect().top - list.getBoundingClientRect().top - restorePosition.offset;
        restorePosition = undefined;
        return;
      }
    }
  }
  if (follow) list.scrollTop = list.scrollHeight;
  else if (anchor?.isConnected && offset !== undefined) list.scrollTop += anchor.getBoundingClientRect().top - offset;
  readingEarlier = false;
});
let readingEarlier = false;
let restorePosition: ReturnType<typeof readPosition>;
// 离开主文档时固定独立窗口中的草稿、未决命令和阅读位置。
function leaving(): void {
  remember();
  savePosition(transcript.value, messages.conversation.value);
}
// 用户申请旧页时保持当前阅读位置。
function earlier(): void { readingEarlier = true; messages.earlier(); }
watch(() => lastTurn.value?.turn, () => { destination.value = 'current'; });
watch(draft, value => {
  try { rememberDraft(value); } catch { notice.value = '当前窗口无法保留刷新前的草稿。'; }
}, { flush: 'sync' });
onMounted(() => {
  recover();
  restorePosition = readPosition();
  window.addEventListener('pagehide', leaving);
  bridge.addEventListener('message', receive);
  read();
  refresh = window.setInterval(read, 250);
});
onUnmounted(() => {
  window.removeEventListener('pagehide', leaving);
  window.clearInterval(refresh);
  bridge.removeEventListener('message', receive);
});
</script>

<template>
  <ManagementShell>
    <template #status>
      <span id="dialogue-state" role="status" :data-ready="ready" :data-accepting="accepting && messages.subscribed.value && !pending">{{ panelStatus }}</span>
    </template>
    <template #room="{ active }">
      <RoomStateView :room="room" :conversation="conversation" :room-current="roomCurrent" :conversation-current="ready">
        <template #mailbox><ResidentRoomMailbox :active="active" :connected="ready && shutdown === 'idle'" /></template>
      </RoomStateView>
    </template>
    <template #settings="{ active }">
      <HostSettings :active="active" :instance="ready && messages.subscribed.value && shutdown === 'idle' ? messages.instance.value : ''" />
      <PluginSettings :active="active" :connected="ready && shutdown === 'idle'" />
      <h3 class="group-title">后台运行</h3>
      <p class="source-note">关闭管理中心后，大肥鱼会继续工作。</p>
      <button id="host-stop" type="button" class="quiet-action" :disabled="!ready || shutdown !== 'idle'" @click="askStop">退出大肥鱼</button>
      <p v-if="shutdown !== 'idle'" id="host-shutdown-status" role="status" class="source-note">
        <template v-if="shutdown === 'stopping'">后台已接收退出请求，正在收尾。{{ lastWait }}<template v-if="longWait">已等待 10 秒以上，可查看下方日志或关闭此窗口。</template></template>
        <template v-else-if="shutdown === 'exited'">{{ connectionNotice }}</template>
        <template v-else-if="shutdown === 'unknown'">退出回执未确认，可用终端查询本体状态。</template>
        <template v-else>正在提交退出请求…</template>
      </p>
      <LogSettings v-if="active" />
    </template>
    <h2 class="chat-title">和大肥鱼聊天</h2>
    <section class="conversation-space" aria-label="主会话">
      <ol ref="transcript" class="transcript" aria-label="对话" aria-live="polite">
        <li v-if="messages.error.value || messages.more.value" class="history-control">
          <template v-if="messages.error.value"><span role="status">{{ messages.error.value }}</span> <button type="button" @click="messages.retry">重试</button></template>
          <button v-else id="history-earlier" type="button" :disabled="messages.loadingOlder.value" @click="earlier">{{ messages.loadingOlder.value ? '正在读取…' : '查看更早消息' }}</button>
        </li>
        <template v-for="entry in timeline" :key="entry.key">
        <li v-if="'row' in entry" class="history-message" :data-entry="entry.key">
          <div class="message" :class="entry.row.sender === 'assistant' ? 'incoming' : 'outgoing'" :data-stored-message="entry.row.message">
            <b>{{ entry.row.sender === 'assistant' ? '大肥鱼' : '你' }} <MessageTime :record="entry.row" /></b><p>{{ entry.row.text }}</p>
            <MessageReceipt v-if="entry.row.sender === 'user'" :record="entry.row" :lookup="messages.receipt" />
          </div>
        </li>
        <li v-else class="turn" :data-entry="entry.key" :data-turn="entry.turn.turn" :data-result="entry.turn.result" :data-exit="entry.turn.exit" :data-queued-message="entry.turn.queuedMessage">
          <div class="message outgoing" :data-stored-message="entry.prompt?.message"><b>你<span v-if="entry.turn.queuedMessage !== '0'" class="queue-origin">从待发队列发出</span> <MessageTime :record="entry.prompt" /></b><p>{{ entry.prompt?.text ?? entry.turn.prompt }}</p></div>
          <div v-for="input in entry.turn.inputs" :key="input.message" class="message outgoing turn-input" :data-stored-message="entry.inputs.get(input.message)?.message" :data-message="input.message" :data-state="input.state" :data-step="input.modelStep">
            <b>本轮补充 <MessageTime :record="entry.inputs.get(input.message)" /></b><p>{{ entry.inputs.get(input.message)?.text ?? input.text }}</p>
            <div class="input-receipt"><span>{{ inputText[input.state] }}</span>
              <button v-if="input.state !== 'submitted' && input.state !== 'waiting'" type="button" @click="reuseInput(input)">加入草稿</button>
            </div>
          </div>
          <div v-for="answer in entry.answers" :key="answer.message" class="message incoming" :data-stored-message="answer.message"><b>大肥鱼 <MessageTime :record="answer" /></b><p>{{ answer.text }}</p></div>
          <div v-if="!entry.answers.length || entry.turn.result !== 'succeeded'" class="turn-status">{{ entry.turn.result === 'succeeded' && entry.turn.answer.trim() ? '本轮已完成，答复投递待核对' : statusText[entry.turn.result] }}</div>
          <small v-if="entry.turn.result !== 'pending' && entry.turn.exit === 'pending'" class="turn-status">本轮仍在收尾</small>
        </li>
        </template>
      </ol>
    </section>
    <details v-if="queue.length" class="queue-panel" :data-paused="queuePaused" open>
      <summary><strong>待发消息</strong><span>{{ queuePaused ? '已暂停' : '按顺序发送' }} · {{ queue.length }} 条</span></summary>
      <ol class="queue-list" aria-label="等待下一轮的消息">
        <li v-for="(item, index) in queue" :key="item.message" :data-message="item.message">
          <span>{{ item.text }}</span>
          <button type="button" class="queue-remove" :disabled="!!pending" :aria-label="`移除第 ${index + 1} 条待发消息`" @click="editQueue('remove', item)">移除</button>
        </li>
      </ol>
      <button v-if="queuePaused && queue[0]" id="dialogue-resume" type="button" class="queue-resume" :disabled="!!pending" @click="editQueue('resume', queue[0])">继续发送</button>
    </details>
    <form class="panel-compose" @submit.prevent="send">
      <textarea id="dialogue-draft" ref="composer" v-model="draft" rows="2" :maxlength="byteLimit" aria-label="给大肥鱼的消息" placeholder="给大肥鱼发消息" @keydown.enter="enter"></textarea>
      <button id="dialogue-send" class="primary-action" type="submit" :disabled="!canSend">{{ appendMode ? '追加本轮' : queueMode ? '排到下一轮' : '发送' }}</button>
      <div v-if="turns.length" class="compose-tools" role="group" aria-label="聊天工具">
        <fieldset v-if="canChoose" class="send-destination">
          <legend class="visually-hidden">忙时消息去向</legend>
          <label><input v-model="destination" type="radio" name="destination" value="current"><span>本轮补充</span></label>
          <label><input v-model="destination" type="radio" name="destination" value="next"><span>下一轮</span></label>
        </fieldset>
        <button v-if="lastTurn?.exit === 'pending'" id="dialogue-stop" class="stop-action" type="button" :disabled="!canStop" @click="stop">停止回复</button>
        <button id="dialogue-clear" class="quiet-action" type="button" :disabled="!canClear" @click="askClear">重置上下文</button>
      </div>
      <output v-if="tooLong || connectionNotice || notice" aria-live="polite">{{ tooLong ? '消息不能超过 1 MiB。' : connectionNotice || notice }}</output>
    </form>
    <dialog ref="clearDialog" class="clear-dialog" aria-labelledby="clear-title" @close="clearTarget = undefined">
      <h2 id="clear-title">重置当前对话上下文？</h2>
      <p>下一次发送将使用新的对话上下文。已保存的聊天记录仍可查阅。<template v-if="draft">输入框里的草稿会保留。</template><template v-if="queue.length">{{ queue.length }} 条待发消息也会保留。</template></p>
      <div class="dialog-actions">
        <button class="quiet-action" type="button" autofocus @click="clearDialog?.close()">继续当前对话</button>
        <button id="dialogue-confirm-clear" class="primary-action" type="button" @click="confirmClear">确认重置</button>
      </div>
    </dialog>
  </ManagementShell>
  <dialog ref="shutdownDialog" class="clear-dialog" aria-labelledby="host-stop-title">
    <h2 id="host-stop-title">退出大肥鱼？</h2>
    <p>所有管理窗口都会断开，正在进行的任务将开始收尾。</p>
    <div class="dialog-actions"><button id="host-stay" type="button" class="quiet-action" autofocus @click="shutdownDialog?.close()">继续运行</button>
      <button id="host-confirm-stop" type="button" class="primary-action" @click="stopHost">确认退出</button></div>
  </dialog>
</template>

<style src="./main-panel.css"></style>
