<script lang="ts">
export interface StateReading {
  readonly source: string;
  readonly identity: string;
  readonly revision: string;
  readonly time: number;
  readonly fields: Readonly<Record<string, string>>;
  readonly binding?: { readonly instance: string; readonly body: string };
}
</script>

<script setup lang="ts">
import { computed, shallowRef, watch } from 'vue';

const props = defineProps<{
  room?: StateReading;
  conversation?: StateReading;
  roomCurrent: boolean;
  conversationCurrent: boolean;
}>();
const groups = computed(() => [
  { id: 'room', name: '本体书房', value: props.room, current: props.roomCurrent },
  { id: 'conversation', name: '主会话', value: props.conversation, current: props.conversationCurrent },
]);
interface Change { id: number; source: string; revision: string; time: number; text: string }
const history = shallowRef<readonly Change[]>([]);
const clock = new Intl.DateTimeFormat('zh-CN', { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });
let changeId = 0;

// 以首个有效快照作基线，只记录本窗口实际收到的字段差异。
// 步骤：核对来源身份与修订 → 标明断档或变更值 → 有界保留最近记录。
function observe(next: StateReading | undefined, previous: StateReading | undefined): void {
  if (!next || !previous) return;
  const notes: string[] = [];
  if (next.identity !== previous.identity) {
    notes.push('来源实例已更换，已重新读取状态');
  } else {
    if (BigInt(next.revision) > BigInt(previous.revision) + 1n)
      notes.push(`从修订 ${previous.revision} 更新至 ${next.revision}，中间变化未观察到`);
    for (const [label, value] of Object.entries(next.fields)) {
      if (previous.fields[label] !== value) notes.push(`${label}：${previous.fields[label] ?? '未知'} → ${value}`);
    }
  }
  if (!notes.length) return;
  const batch = notes.map(text => ({ id: ++changeId, source: next.source, revision: next.revision, time: next.time, text }));
  history.value = [...batch, ...history.value].slice(0, 32);
}
watch(() => props.room, observe);
watch(() => props.conversation, observe);
</script>

<template>
  <h2 class="view-title">房间</h2>
  <h3 class="group-title">最新状态</h3>
  <section v-for="group in groups" :key="group.id" class="readout" :data-source="group.id" :data-current="!!group.value && group.current">
    <header><h4>{{ group.name }}</h4>
      <span class="source-note">{{ group.value ? (group.current ? '已同步' : '数据过期') : '等待读取' }}</span>
    </header>
    <template v-if="group.value">
      <dl class="state-fields">
        <template v-for="(value, label) in group.value.fields" :key="label">
          <dt>{{ label }}</dt>
          <dd :id="label === '当前位置' ? 'room-location' : undefined"
              :data-room="label === '当前位置' ? group.value.binding?.instance : undefined"
              :data-body="label === '当前位置' ? group.value.binding?.body : undefined">{{ value }}</dd>
        </template>
      </dl>
      <p class="readout-meta">修订 {{ group.value.revision }} · 本窗接收 {{ clock.format(group.value.time) }}</p>
    </template>
  </section>
  <h3 class="group-title">最近变化</h3>
  <ol v-if="history.length" class="changes" aria-label="本窗口最近观察到的变化">
    <li v-for="change in history" :key="change.id"><span>{{ change.text }}</span>
      <small>{{ change.source }} · 修订 {{ change.revision }} · {{ clock.format(change.time) }}</small></li>
  </ol>
  <p v-else class="source-note">本窗口尚未观察到变化。</p>
</template>

<style scoped>
.readout { padding: 13px 16px 10px; margin: 8px 0; border: 1px solid var(--line); border-radius: 10px; background: var(--paper) }
.readout header { display: flex; align-items: baseline; justify-content: space-between; gap: 12px }
.readout h4 { margin: 0; font-size: 14px }
.readout .source-note, .readout-meta, .changes small { font-size: 12px; color: var(--muted) }
.readout[data-current=false] .source-note { color: #875327 }
.readout-meta { margin: 8px 0 0; font-variant-numeric: tabular-nums }
.readout .state-fields { font-size: 13px; column-gap: 12px }
.readout dt, .readout dd { padding: 4px 0 }
.changes { list-style: none; margin: 0; padding: 0 }
.changes li { padding: 8px 0; border-bottom: 1px solid var(--line); font-size: 13px; overflow-wrap: anywhere }
.changes small { display: block; margin-top: 3px }
</style>
