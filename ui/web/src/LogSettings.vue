<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue';

const bridge = window.chrome.webview;
const readId = crypto.randomUUID();
const folder = ref('');
const status = ref('');
const outcome = ref('');
const opening = ref(false);
let openId = '', refresh = 0, deadline = 0;

// 只查询当前受信窗口所属的日志，不随请求提供路径。
function read(): void {
  bridge.postMessage({ type: 'logs.read', version: '1', request: readId });
}

// 对一次用户点击签发打开请求，等待超时后保留结果未知的反馈。
// 步骤：固定本次身份 → 提交明确操作 → 有界等待原回执。
function openFolder(): void {
  if (opening.value) return;
  opening.value = true;
  outcome.value = '';
  openId = crypto.randomUUID();
  bridge.postMessage({ type: 'logs.open', version: '1', request: openId });
  deadline = window.setTimeout(() => { opening.value = false; outcome.value = '打开请求的结果尚未确认。'; }, 8000);
}

// 按本页的查询或打开身份接收原生状态，普通查询保留上一次打开的结果。
// 步骤：校验来源请求和协议 → 更新实际写入状态 → 显示系统对打开操作的回执。
function receive(event: MessageEvent<unknown>): void {
  if (!event.data || typeof event.data !== 'object') return;
  const value = event.data as Record<string, unknown>;
  const isOpen = !!openId && value.request === openId;
  if (value.version !== '1' || (!isOpen && value.request !== readId)) return;
  if (isOpen) { window.clearTimeout(deadline); opening.value = false; }
  if (value.event === 'management.error') {
    if (isOpen) outcome.value = '打开日志目录的请求未能完成。';
    else status.value = '暂时无法读取当前窗口的日志状态。';
    return;
  }
  if (value.event !== 'logs.status' || typeof value.folder !== 'string' || typeof value.writeError !== 'number' ||
      typeof value.written !== 'boolean') return;
  folder.value = value.folder;
  status.value = value.writeError ? `当前窗口写入日志失败，系统错误码 ${value.writeError}。`
    : value.written ? '当前窗口已写入运行日志。' : '正在等候首条日志写入。';
  if (isOpen && typeof value.openError === 'number')
    outcome.value = value.openError ? `无法打开日志目录，系统错误码 ${value.openError}。` : '系统已接收打开目录请求。';
}

onMounted(() => {
  bridge.addEventListener('message', receive);
  read();
  refresh = window.setInterval(read, 2500);
});
onUnmounted(() => {
  bridge.removeEventListener('message', receive);
  window.clearInterval(refresh);
  window.clearTimeout(deadline);
});
</script>

<template>
  <h3 class="group-title">诊断</h3>
  <p v-if="folder" id="log-folder" class="source-note">{{ folder }}</p>
  <button id="open-logs" type="button" class="quiet-action" :disabled="opening" @click="openFolder">打开日志目录</button>
  <p id="log-status" role="status" class="source-note">{{ status }} {{ outcome }}</p>
</template>
