<script setup lang="ts">
import { onMounted, onUnmounted, watch } from 'vue';
import RoomMailbox from './RoomMailbox.vue';
import { useMailboxObserver } from './mailbox-observer';

const props = defineProps<{ connected: boolean; active: boolean }>();
const bridge = window.chrome.webview;
const observer = useMailboxObserver(fields => bridge.postMessage(fields));
let timer = 0;

// 只把原生回包交给当前文档自己的观察器。
function receive(event: MessageEvent<unknown>): void { observer.receive(event.data); }
// 房间切换或原连接失效时立即更新只读范围。
function update(): void { observer.context(props.connected, props.active); }
watch(() => [props.connected, props.active], update);
onMounted(() => {
  bridge.addEventListener('message', receive);
  update();
  timer = window.setInterval(observer.tick, 500);
});
onUnmounted(() => {
  window.clearInterval(timer);
  bridge.removeEventListener('message', receive);
  observer.context(false, false);
});
</script>

<template>
  <RoomMailbox :view="observer.view.value" @visibility="observer.visibility" @select="observer.select" @retry="observer.retry" />
</template>
