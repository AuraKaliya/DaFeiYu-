<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, shallowRef, watch } from 'vue';
import study from './art/study.png';

type Operation = 'pending' | 'succeeded' | 'failed' | 'cancelled';
type CommandState = 'unavailable' | 'queued' | 'accepted' | 'busy' | 'rejected' | 'conflict' | 'expired' | 'stale';
interface Turn { readonly session: string; readonly turn: string; readonly prompt: string; readonly answer: string; readonly result: Operation; readonly exit: Operation }
interface Receipt { readonly command: string; readonly state: CommandState }
type Target = Pick<Turn, 'session' | 'turn'>;
type Pending = { readonly id: string; readonly action: 'send'; readonly text: string } |
  { readonly id: string; readonly action: 'cancel' | 'clear' };

const operations: ReadonlySet<unknown> = new Set<Operation>(['pending', 'succeeded', 'failed', 'cancelled']);
const commandStates: ReadonlySet<unknown> = new Set<CommandState>(['unavailable', 'queued', 'accepted', 'busy', 'rejected', 'conflict', 'expired', 'stale']);
const statusText: Record<Operation, string> = { pending: '正在回复', succeeded: '本轮已完成', failed: '本轮回复失败', cancelled: '已停止回复' };
const rejection: Record<Exclude<CommandState, 'queued' | 'accepted'>, string> = {
  unavailable: '会话暂时无法使用。', busy: '会话正在处理其他操作。',
  rejected: '这次操作未被接纳。', conflict: '此命令编号已用于其他操作。', expired: '本次回执已过期。',
  stale: '原回合已结束或发生变化。',
};
const byteLimit = 1024 * 1024;
const encoder = new TextEncoder();
const bridge = window.chrome.webview;
const scope = `${crypto.randomUUID()}-`;
const turns = shallowRef<readonly Turn[]>([]);
const pending = shallowRef<Pending>();
const transcript = ref<HTMLOListElement>();
const composer = ref<HTMLTextAreaElement>();
const clearDialog = ref<HTMLDialogElement>();
const clearTarget = shallowRef<Target>();
const draft = ref('');
const notice = ref('');
const ready = ref(false);
const accepting = ref(false);
const tooLong = computed(() => encoder.encode(draft.value).length > byteLimit);
const canSend = computed(() => accepting.value && !pending.value && !!draft.value.trim() && !tooLong.value);
const lastTurn = computed(() => turns.value.at(-1));
const canStop = computed(() => ready.value && !pending.value && lastTurn.value?.result === 'pending');
const canClear = computed(() => accepting.value && !pending.value && !!lastTurn.value);
// 区分页面提交、后端收尾与可接纳下一轮，上一轮的结果留在原条目。
const panelStatus = computed(() => {
  if (!ready.value) return '正在连接';
  if (pending.value) return { send: '正在提交', cancel: '已请求停止', clear: '正在清空' }[pending.value.action];
  if (accepting.value) return '可以聊天';
  return lastTurn.value?.result === 'cancelled' && lastTurn.value.exit === 'pending' ? '正在收尾' : '正在处理对话';
});
let command = 0n;
let request = 0, latestSnapshot = 0;
let revision = '';
let refresh = 0;

// 先确认浏览器回包是可读取字段的对象。
function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

// 验证由原生端签发的十进制身份，没有转换为有精度限制的 JS 整数。
function isDecimal(value: unknown): value is string {
  return typeof value === 'string' && /^\d{1,20}$/.test(value);
}

// 只接受同时具有原会话身份、正文及两项运行状态的显示条目。
function isTurn(value: unknown): value is Turn {
  return isRecord(value) && isDecimal(value.session) && isDecimal(value.turn) &&
    typeof value.prompt === 'string' && typeof value.answer === 'string' && operations.has(value.result) && operations.has(value.exit);
}

// 验证窗口命令回执，以原命令号关联本地草稿。
function isReceipt(value: unknown): value is Receipt {
  return isRecord(value) && isDecimal(value.command) && commandStates.has(value.state);
}

// 给当前页面的所有命令使用独立请求身份，便于拒绝其他文档的回包。
function post(fields: Readonly<Record<string, string>>): void {
  bridge.postMessage({ ...fields, version: '1', request: `${scope}${++request}` });
}

// 按已读修订查询快照，等待原提交时同时查询同号回执。
function read(): void {
  post({ type: 'dialogue.read', revision, command: pending.value?.id ?? '' });
}

// 只在原发送获接纳时清掉未改动的原稿，控制命令由正式快照呈现结果。
// 步骤：核对待交付身份 → 按命令结算草稿与焦点 → 显示宿主拒绝原因。
function settle(receipt: Receipt): void {
  const current = pending.value;
  if (!current || current.id !== receipt.command || receipt.state === 'queued') return;
  pending.value = undefined;
  if (receipt.state === 'accepted') {
    if (current.action === 'send' && draft.value === current.text) draft.value = '';
    if (current.action === 'clear') composer.value?.focus();
  } else notice.value = rejection[receipt.state];
}

// 将原生快照投影为当前页面的对话，过滤协议不符及迟到的旧快照。
// 步骤：核对页面请求 → 验证快照字段并更新正文 → 结算匹配的窗口命令。
function receive(event: MessageEvent<unknown>): void {
  const value = event.data;
  if (!isRecord(value) || value.version !== '1' || typeof value.request !== 'string' || !value.request.startsWith(scope)) return;
  if (value.event === 'dialogue.snapshot') {
    const sequence = Number(value.request.slice(scope.length));
    if (!Number.isSafeInteger(sequence) || sequence <= latestSnapshot || typeof value.ready !== 'boolean' ||
        typeof value.accepting !== 'boolean' || !isDecimal(value.latestCommand) || typeof value.revision !== 'string' ||
        (value.turns !== null && (!Array.isArray(value.turns) || !value.turns.every(isTurn)))) return;
    latestSnapshot = sequence;
    ready.value = value.ready;
    accepting.value = value.accepting;
    const latestCommand = BigInt(value.latestCommand);
    if (latestCommand > command) command = latestCommand;
    if (value.turns !== null) {
      revision = value.revision;
      turns.value = value.turns;
    }
  } else if (value.event !== 'dialogue.receipt') return;
  if (isReceipt(value.receipt)) settle(value.receipt);
}

// 在页面重复提交前固定一个原命令，原稿等实际接纳再清除。
// 步骤：核对接纳和草稿 → 标记待交付 → 发给原生主文档入口。
function send(): void {
  if (!canSend.value) return;
  const submission = { id: String(++command), action: 'send' as const, text: draft.value };
  pending.value = submission;
  accepting.value = false;
  notice.value = '';
  post({ type: 'dialogue.submit', command: submission.id, text: submission.text });
}

// 发出绑定原会话和回合的控制命令，并沿同一编号查询回执。
function control(action: 'cancel' | 'clear', target: Target): void {
  const id = String(++command);
  pending.value = { id, action };
  notice.value = '';
  post({ type: 'dialogue.' + action, command: id, session: target.session, turn: target.turn });
}

// 对当前显示的原回合发出停止，草稿仍可编辑。
function stop(): void {
  if (canStop.value && lastTurn.value) control('cancel', lastTurn.value);
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
  control('clear', target);
}

// 区分正常发送、输入法确认和 Shift+Enter 换行。
function enter(event: KeyboardEvent): void {
  if (event.isComposing || event.keyCode === 229 || event.shiftKey) return;
  event.preventDefault();
  send();
}

watch(turns, () => { if (transcript.value) transcript.value.scrollTop = transcript.value.scrollHeight; }, { flush: 'post' });
onMounted(() => {
  bridge.addEventListener('message', receive);
  read();
  refresh = window.setInterval(read, 250);
});
onUnmounted(() => {
  window.clearInterval(refresh);
  bridge.removeEventListener('message', receive);
});
</script>

<template>
  <main class="main-panel">
    <header class="panel-header">
      <div class="panel-identity">
        <h1>大肥鱼</h1>
        <span id="dialogue-state" role="status" :data-ready="ready" :data-accepting="accepting && !pending">{{ panelStatus }}</span>
      </div>
      <button v-if="turns.length" id="dialogue-clear" class="quiet-action" type="button" :disabled="!canClear" @click="askClear">清空对话</button>
    </header>
    <section class="conversation-space" aria-label="主会话">
      <div v-if="ready && !turns.length" class="room-still" aria-hidden="true"><img :src="study" alt=""></div>
      <ol ref="transcript" class="transcript" aria-label="对话" aria-live="polite">
        <li v-for="turn in turns" :key="`${turn.session}/${turn.turn}`" class="turn" :data-turn="turn.turn" :data-result="turn.result" :data-exit="turn.exit">
          <div class="message outgoing"><b>你</b><p>{{ turn.prompt }}</p></div>
          <div v-if="turn.answer" class="message incoming"><b>大肥鱼</b><p>{{ turn.answer }}</p></div>
          <div v-else class="turn-status">{{ statusText[turn.result] }}</div>
          <small v-if="turn.result !== 'pending' && turn.exit === 'pending'" class="turn-status">本轮仍在收尾</small>
        </li>
      </ol>
    </section>
    <form class="panel-compose" @submit.prevent="send">
      <textarea id="dialogue-draft" ref="composer" v-model="draft" rows="2" :maxlength="byteLimit" aria-label="给大肥鱼的消息" placeholder="给大肥鱼发消息" @keydown.enter="enter"></textarea>
      <div class="compose-actions">
        <button v-if="lastTurn?.exit === 'pending'" id="dialogue-stop" class="stop-action" type="button" :disabled="!canStop" @click="stop">停止回复</button>
        <button id="dialogue-send" class="primary-action" type="submit" :disabled="!canSend">发送</button>
      </div>
      <output v-if="tooLong || notice" aria-live="polite">{{ tooLong ? '消息不能超过 1 MiB。' : notice }}</output>
    </form>
    <dialog ref="clearDialog" class="clear-dialog" aria-labelledby="clear-title" @close="clearTarget = undefined">
      <h2 id="clear-title">清空本次对话？</h2>
      <p>下一次发送将从空白对话开始。<template v-if="draft">输入框里的草稿会保留。</template></p>
      <div class="dialog-actions">
        <button class="quiet-action" type="button" autofocus @click="clearDialog?.close()">保留对话</button>
        <button id="dialogue-confirm-clear" class="primary-action" type="button" @click="confirmClear">确认清空</button>
      </div>
    </dialog>
  </main>
</template>

<style src="./main-panel.css"></style>
