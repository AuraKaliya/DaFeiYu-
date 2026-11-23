<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, shallowRef, watch } from 'vue';

type Operation = 'pending' | 'succeeded' | 'failed' | 'cancelled';
type CommandState = 'unavailable' | 'queued' | 'accepted' | 'busy' | 'rejected' | 'conflict' | 'expired';
interface Turn { readonly session: string; readonly turn: string; readonly prompt: string; readonly answer: string; readonly result: Operation; readonly exit: Operation }
interface Receipt { readonly command: string; readonly state: CommandState }
interface Pending { readonly id: string; readonly text: string }

const operations: ReadonlySet<unknown> = new Set<Operation>(['pending', 'succeeded', 'failed', 'cancelled']);
const commandStates: ReadonlySet<unknown> = new Set<CommandState>(['unavailable', 'queued', 'accepted', 'busy', 'rejected', 'conflict', 'expired']);
const statusText: Record<Operation, string> = { pending: '正在回复', succeeded: '本轮已完成', failed: '本轮回复失败', cancelled: '本轮已取消' };
const rejection: Record<Exclude<CommandState, 'queued' | 'accepted'>, string> = {
  unavailable: '暂时无法发送，请稍后再试。', busy: '大肥鱼正在处理一轮对话，草稿已保留。',
  rejected: '发送未接纳，草稿已保留。', conflict: '本次提交编号已使用，草稿已保留。', expired: '本次回执已过期，草稿已保留。',
};
const byteLimit = 1024 * 1024;
const encoder = new TextEncoder();
const bridge = window.chrome.webview;
const scope = `${crypto.randomUUID()}-`;
const turns = shallowRef<readonly Turn[]>([]);
const pending = shallowRef<Pending>();
const transcript = ref<HTMLOListElement>();
const draft = ref('');
const notice = ref('');
const ready = ref(false);
const accepting = ref(false);
const tooLong = computed(() => encoder.encode(draft.value).length > byteLimit);
const canSend = computed(() => accepting.value && !pending.value && !!draft.value.trim() && !tooLong.value);
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

// 原命令获接纳时清掉已发送原稿，拒绝时保留当前编辑内容。
// 步骤：核对待交付身份 → 确认业务接纳或拒绝 → 更新输入与反馈。
function settle(receipt: Receipt): void {
  const current = pending.value;
  if (!current || current.id !== receipt.command || receipt.state === 'queued') return;
  pending.value = undefined;
  if (receipt.state === 'accepted') {
    if (draft.value === current.text) draft.value = '';
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
  const submission = { id: String(++command), text: draft.value };
  pending.value = submission;
  accepting.value = false;
  notice.value = '';
  post({ type: 'dialogue.submit', command: submission.id, text: submission.text });
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
      <h1>大肥鱼</h1>
      <span id="dialogue-state" role="status" :data-ready="ready" :data-accepting="accepting && !pending">
        {{ !ready ? '正在连接' : pending ? '正在提交' : accepting ? '可以聊天' : '正在处理对话' }}
      </span>
    </header>
    <ol ref="transcript" class="transcript" aria-label="对话" aria-live="polite">
      <li v-for="turn in turns" :key="`${turn.session}/${turn.turn}`" class="turn" :data-turn="turn.turn" :data-result="turn.result" :data-exit="turn.exit">
        <div class="message outgoing"><b>你</b><p>{{ turn.prompt }}</p></div>
        <div v-if="turn.answer" class="message incoming"><b>大肥鱼</b><p>{{ turn.answer }}</p></div>
        <div v-else class="turn-status">{{ statusText[turn.result] }}</div>
        <small v-if="turn.result !== 'pending' && turn.exit === 'pending'" class="turn-status">本轮仍在收尾</small>
      </li>
    </ol>
    <form class="panel-compose" @submit.prevent="send">
      <textarea id="dialogue-draft" v-model="draft" rows="2" :maxlength="byteLimit" aria-label="给大肥鱼的消息" placeholder="给大肥鱼发消息" @keydown.enter="enter"></textarea>
      <button id="dialogue-send" type="submit" :disabled="!canSend">发送</button>
      <output v-if="tooLong || notice" aria-live="polite">{{ tooLong ? '消息不能超过 1 MiB。' : notice }}</output>
    </form>
  </main>
</template>

<style src="./main-panel.css"></style>
