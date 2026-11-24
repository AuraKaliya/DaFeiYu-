<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, shallowRef, watch } from 'vue';
import ManagementShell from './ManagementShell.vue';
import RoomStateView, { type StateReading } from './RoomStateView.vue';
import { recoverDialogue, rememberDialogue, rememberDraft, type IntentAction, type WireCommand } from './dialogue-recovery';

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
const composer = ref<HTMLTextAreaElement>();
const clearDialog = ref<HTMLDialogElement>();
const clearTarget = shallowRef<Target>();
const draft = ref('');
const destination = ref<'current' | 'next'>('current');
const notice = ref('');
const connectionNotice = ref('');
const shutdownDialog = ref<HTMLDialogElement>();
const shutdown = ref<'idle' | 'requested' | 'stopping' | 'unknown'>('idle');
let shutdownRequest = '';
const ready = ref(false);
const accepting = ref(false);
const tooLong = computed(() => encoder.encode(draft.value).length > byteLimit);
const lastTurn = computed(() => turns.value.at(-1));
const queueMode = computed(() => lastTurn.value?.exit === 'pending');
const canChoose = computed(() => queueMode.value && lastTurn.value?.result === 'pending' && lastTurn.value.acceptsInput);
const appendMode = computed(() => canChoose.value && destination.value === 'current');
const canSend = computed(() => ready.value && (accepting.value || queueMode.value) && !pending.value && !!draft.value.trim() && !tooLong.value);
const stoppedCurrent = computed(() => !!stopTarget.value && stopTarget.value.session === lastTurn.value?.session && stopTarget.value.turn === lastTurn.value?.turn);
const canStop = computed(() => ready.value && !pendingStop.value && !stoppedCurrent.value && lastTurn.value?.result === 'pending');
const canClear = computed(() => accepting.value && !pending.value && !!lastTurn.value);
// 区分页面提交、后端收尾与可接纳下一轮，上一轮的结果留在原条目。
const panelStatus = computed(() => {
  if (shutdown.value === 'stopping') return '本体正在收尾';
  if (shutdown.value === 'requested') return '正在请求退出';
  if (connectionNotice.value) return '本体连接中断';
  if (!ready.value) return conversation.value ? '连接中断' : '正在连接';
  if (lastTurn.value?.result === 'cancelled' && lastTurn.value.exit === 'pending') return '正在收尾';
  if (stoppedCurrent.value && lastTurn.value?.result === 'pending') return '已请求停止';
  if (pending.value) return { send: '正在提交', enqueue: '正在排队', append: '正在提交补充', clear: '正在清空', resume: '正在恢复', remove: '正在移除' }[pending.value.action];
  if (accepting.value) return '可以聊天';
  return '正在处理对话';
});
let command = 0n;
let request = 0, latestSnapshot = 0;
let revision = '';
let refresh = 0;
const outstanding = new Map<string, WireCommand>();
const reconnecting = new Set<string>();

// 在提交、结算及离开页面时固定草稿和未决原件。
function remember(): void {
  try { rememberDialogue(draft.value, outstanding.values()); }
  catch { notice.value = '当前窗口无法保留刷新前的草稿，请先等待操作回执。'; }
}

// 从同一窗口历史项恢复未决身份，等原生快照核对后决定是否补投原命令。
// 步骤：还原草稿 → 固定命令水位及两个槽位 → 等待正式回执。
function recover(): void {
  const saved = recoverDialogue();
  draft.value = saved.draft;
  for (const fields of saved.commands) {
    const id = fields.command;
    outstanding.set(id, fields);
    reconnecting.add(id);
    if (BigInt(id) > command) command = BigInt(id);
    const action = (fields.type === 'dialogue.submit' ? 'send' : fields.type.slice(9)) as IntentAction;
    if (action === 'cancel') {
      pendingStop.value = id;
      stopTarget.value = { session: fields.session, turn: fields.turn };
    } else if (action === 'send' || action === 'append' || action === 'enqueue') pending.value = { id, action, text: fields.text };
    else pending.value = { id, action };
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
function post(fields: Readonly<Record<string, string>>): void {
  if (fields.command && fields.type !== 'dialogue.read') {
    outstanding.set(fields.command, fields);
    remember();
  }
  bridge.postMessage({ ...fields, version: '1', request: `${scope}${++request}` });
}

// 查询两类快照与原命令回执，失联时保留最后可信的可见值。
// 步骤：检查接收时效 → 查询对话与未决回执 → 按房间频率补读。
function read(): void {
  if (shutdown.value === 'stopping') return;
  const now = performance.now();
  if (now - dialogueConfirmed > freshnessMs) { ready.value = false; accepting.value = false; }
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

// 将原生快照投影为当前页面的对话，过滤协议不符及迟到的旧快照。
// 步骤：核对页面请求 → 验证快照字段并更新正文 → 结算匹配的窗口命令。
function receive(event: MessageEvent<unknown>): void {
  const value = event.data;
  if (!isRecord(value) || value.version !== '1' || typeof value.request !== 'string' || !value.request.startsWith(scope)) return;
  const sequence = Number(value.request.slice(scope.length));
  if (!Number.isSafeInteger(sequence) || sequence <= 0) return;
  if (value.request === shutdownRequest && value.event === 'host.stopping' && value.state === 'stopping') {
    shutdown.value = 'stopping';
    ready.value = false;
    accepting.value = false;
    roomCurrent.value = false;
    return;
  }
  if (shutdown.value === 'stopping') return;
  if (value.event === 'management.error') {
    if (value.request === shutdownRequest) shutdown.value = 'unknown';
    if (sequence < latestSnapshot) return;
    ready.value = false;
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
    if (value.ready && reconnecting.size) {
      // 宿主水位尚未登记的原件才按原编号补投一次；其他原件继续只读查询。
      for (const [id, fields] of outstanding) {
        if (reconnecting.delete(id) && BigInt(id) > latestCommand) post(fields);
      }
    }
  } else if (value.event !== 'dialogue.receipt') return;
  if (isReceipt(value.receipt)) settle(value.receipt);
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
  notice.value = '';
  if (target && queueMode.value) {
    const action = appendMode.value ? 'append' : 'enqueue';
    pending.value = { id, action, text };
    post({ type: 'dialogue.' + action, command: id, session: target.session, turn: target.turn, text });
  } else {
    pending.value = { id, action: 'send', text };
    accepting.value = false;
    post({ type: 'dialogue.submit', command: id, text });
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
  post({ type: 'dialogue.' + action, command: id, session: item.session, message: item.message });
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
    notice.value = '对话已更新，请重新选择要清空的会话。';
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

watch(turns, () => { if (transcript.value) transcript.value.scrollTop = transcript.value.scrollHeight; }, { flush: 'post' });
watch(() => lastTurn.value?.turn, () => { destination.value = 'current'; });
watch(draft, value => {
  try { rememberDraft(value); } catch { notice.value = '当前窗口无法保留刷新前的草稿。'; }
}, { flush: 'sync' });
onMounted(() => {
  recover();
  window.addEventListener('pagehide', remember);
  bridge.addEventListener('message', receive);
  read();
  refresh = window.setInterval(read, 250);
});
onUnmounted(() => {
  window.removeEventListener('pagehide', remember);
  window.clearInterval(refresh);
  bridge.removeEventListener('message', receive);
});
</script>

<template>
  <ManagementShell>
    <template #status>
      <span id="dialogue-state" role="status" :data-ready="ready" :data-accepting="accepting && !pending">{{ panelStatus }}</span>
    </template>
    <template #room>
      <RoomStateView :room="room" :conversation="conversation" :room-current="roomCurrent" :conversation-current="ready" />
    </template>
    <template #settings>
      <h3 class="group-title">插件管理</h3>
      <p class="source-note">插件目录尚未接入此视图。</p>
      <h3 class="group-title">后台运行</h3>
      <p class="source-note">关闭管理中心后，大肥鱼会继续工作。</p>
      <button id="host-stop" type="button" class="quiet-action" :disabled="!ready || shutdown !== 'idle'" @click="shutdownDialog?.showModal()">退出大肥鱼</button>
      <p v-if="shutdown !== 'idle'" id="host-shutdown-status" role="status" class="source-note">{{ shutdown === 'stopping' ? '后台已接收退出请求，正在收尾。' : shutdown === 'unknown' ? '退出回执未确认，可用终端查询本体状态。' : '正在提交退出请求…' }}</p>
      <dialog ref="shutdownDialog" class="clear-dialog" aria-labelledby="host-stop-title">
        <h2 id="host-stop-title">退出大肥鱼？</h2>
        <p>所有管理窗口都会断开，正在进行的任务将开始收尾。当前版本的聊天记录只保存在后台进程内。</p>
        <div class="dialog-actions"><button id="host-stay" type="button" class="quiet-action" autofocus @click="shutdownDialog?.close()">继续运行</button>
          <button id="host-confirm-stop" type="button" class="primary-action" @click="stopHost">确认退出</button></div>
      </dialog>
    </template>
    <h2 class="chat-title">和大肥鱼聊天</h2>
    <section class="conversation-space" aria-label="主会话">
      <ol ref="transcript" class="transcript" aria-label="对话" aria-live="polite">
        <li v-for="turn in turns" :key="`${turn.session}/${turn.turn}`" class="turn" :data-turn="turn.turn" :data-result="turn.result" :data-exit="turn.exit" :data-queued-message="turn.queuedMessage">
          <div class="message outgoing"><b>你<span v-if="turn.queuedMessage !== '0'" class="queue-origin">从待发队列发出</span></b><p>{{ turn.prompt }}</p></div>
          <div v-for="input in turn.inputs" :key="input.message" class="message outgoing turn-input" :data-message="input.message" :data-state="input.state" :data-step="input.modelStep">
            <b>本轮补充</b><p>{{ input.text }}</p>
            <div class="input-receipt"><span>{{ inputText[input.state] }}</span>
              <button v-if="input.state !== 'submitted' && input.state !== 'waiting'" type="button" @click="reuseInput(input)">加入草稿</button>
            </div>
          </div>
          <div v-if="turn.answer" class="message incoming"><b>大肥鱼</b><p>{{ turn.answer }}</p></div>
          <div v-else class="turn-status">{{ statusText[turn.result] }}</div>
          <small v-if="turn.result !== 'pending' && turn.exit === 'pending'" class="turn-status">本轮仍在收尾</small>
        </li>
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
        <button id="dialogue-clear" class="quiet-action" type="button" :disabled="!canClear" @click="askClear">清空对话</button>
      </div>
      <output v-if="tooLong || connectionNotice || notice" aria-live="polite">{{ tooLong ? '消息不能超过 1 MiB。' : connectionNotice || notice }}</output>
    </form>
    <dialog ref="clearDialog" class="clear-dialog" aria-labelledby="clear-title" @close="clearTarget = undefined">
      <h2 id="clear-title">清空本次对话？</h2>
      <p>下一次发送将从空白对话开始。<template v-if="draft">输入框里的草稿会保留。</template><template v-if="queue.length">{{ queue.length }} 条待发消息也会保留。</template></p>
      <div class="dialog-actions">
        <button class="quiet-action" type="button" autofocus @click="clearDialog?.close()">保留对话</button>
        <button id="dialogue-confirm-clear" class="primary-action" type="button" @click="confirmClear">确认清空</button>
      </div>
    </dialog>
  </ManagementShell>
</template>

<style src="./main-panel.css"></style>
