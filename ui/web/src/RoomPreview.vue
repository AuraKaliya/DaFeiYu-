<script setup lang="ts">
import { ref } from 'vue';
import room from './art/study.png?no-inline';

const props = defineProps<{ initialBusy: boolean }>();
const busy = ref(props.initialBusy);
const stopped = ref(false);
const draft = ref('');
const expanded = ref(true);
const route = ref<'current' | 'next'>('current');
const decision = ref('');
const notice = ref('');
const messages = ref<{ text: string; state: string }[]>([]);

// 载入指定公开样本，并清理上一个样本中的局部操作。
function selectSample(working: boolean): void {
  busy.value = working; stopped.value = false; expanded.value = true; route.value = 'current';
  decision.value = ''; notice.value = ''; draft.value = ''; messages.value = [];
}
// 记录一次本地输入及其去向，不将正文发送给宿主。
function send(): void {
  const text = draft.value.trim();
  if (!text) return;
  const state = busy.value && !stopped.value ? (route.value === 'current' ? '待带入本轮' : '下一轮排队') : '预览已接收';
  messages.value = [...messages.value.slice(-5), { text, state }];
  notice.value = state; draft.value = ''; expanded.value = true;
}
// 输入法选字和换行期间保留编辑，其余回车发送本地草稿。
function enter(event: KeyboardEvent): void {
  if (event.isComposing || event.keyCode === 229 || event.shiftKey) return;
  event.preventDefault(); send();
}
</script>

<template>
  <main class="room-preview" :data-sample="busy ? 'busy' : 'idle'">
    <header class="mast"><span class="emblem" aria-hidden="true"><i></i></span>
      <h1>大肥鱼管理中心</h1>
      <div class="samples" aria-label="首屏预览样本"><small>首屏预览</small>
        <button id="sample-idle" :aria-pressed="!busy" @click="selectSample(false)">空闲</button>
        <button id="sample-busy" :aria-pressed="busy" @click="selectSample(true)">忙碌</button>
      </div>
    </header>
    <section aria-labelledby="room-title" class="room-card">
      <div class="room-bar"><h2 id="room-title">起居室</h2>
        <span class="presence" :class="{ working: busy && !stopped }"><i></i>{{ busy && !stopped ? '正在前往窗边' : stopped ? '已在途中停下' : '在书桌旁 · 可以聊天' }}</span>
      </div>
      <div class="scene" role="img" :aria-label="busy ? '大肥鱼在书桌和窗边之间，右侧是屏风' : '大肥鱼在书桌前，窗外是青山，桌上有台灯'">
        <img class="backdrop" :src="room" alt=""><span class="resident" :class="{ walking: busy }" aria-hidden="true">大肥鱼</span>
      </div>
    </section>
    <section class="conversation" aria-labelledby="chat-heading">
      <div class="chat-bar"><h2 id="chat-heading">和大肥鱼说话</h2>
        <button id="toggle-chat" class="quiet" aria-controls="chat-body" :aria-expanded="expanded" @click="expanded = !expanded">{{ expanded ? '收起对话' : '展开对话' }}</button>
        <button v-if="busy && !stopped" id="stop-turn" class="stop" @click="stopped = true; notice = '预览已停止，排队消息已暂停'">■ 停止本轮</button>
      </div>
      <div v-show="expanded" id="chat-body">
        <p class="utterance"><b>{{ busy ? '你' : '大肥鱼' }}</b>{{ busy ? '去窗边等我。' : '我在书桌旁。今天想聊点什么？' }}</p>
        <div v-if="busy" class="sample-lines">
          <p><span>{{ stopped ? '可重新发送' : '待带入本轮' }}</span>到窗边以后再告诉我。</p>
          <p><span>{{ stopped ? '队列已暂停' : '下一轮排队' }}</span>等会儿聊聊窗外的风景。</p>
        </div>
        <p v-for="(message, i) in messages" :key="i" class="utterance"><b>{{ stopped && message.state === '下一轮排队' ? '队列已暂停' : stopped && message.state === '待带入本轮' ? '可重新发送' : message.state }}</b>{{ message.text }}</p>
      </div>
      <details v-if="busy && !stopped" class="decision">
        <summary><span>{{ decision ? '已决定' : '待你决定' }}</span>{{ decision || '到窗边以后，要打开台灯吗？' }}</summary>
        <p>你交代的是去窗边等候。开灯另需你的决定。</p>
        <div v-if="!decision"><button @click="decision = '打开台灯'">打开台灯</button> <button @click="decision = '保持原样'">保持原样</button></div>
      </details>
      <form class="compose" @submit.prevent="send">
        <textarea id="room-draft" v-model="draft" rows="1" maxlength="512" aria-label="给大肥鱼的消息" placeholder="给大肥鱼留句话…" @keydown.enter="enter"></textarea>
        <select v-if="busy && !stopped" id="send-to" v-model="route" aria-label="消息去向"><option value="current">追加本轮</option><option value="next">排到下一轮</option></select>
        <button class="send" type="submit" :disabled="!draft.trim()">发送 ↑</button>
      </form>
      <output class="feedback" aria-live="polite">{{ notice }}</output>
    </section>
  </main>
</template>

<style src="./room-preview.css"></style>
