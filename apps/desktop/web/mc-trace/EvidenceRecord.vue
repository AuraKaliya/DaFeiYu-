<script setup lang="ts">
import { computed, ref } from 'vue';
import DataBlock from './DataBlock.vue';
import SourceButton from './SourceButton.vue';
import WorldResult from './WorldResult.vue';
import { statusLabel, tone } from './display.ts';
import { decodedObject, object, objects } from './trace-file.ts';
import { useRecord } from './use-record.ts';
import type { McRecord } from './mc-records.ts';
import type { LineRef } from './trace-file.ts';
import type { LoadedTrace } from './viewer-state.ts';

const props = defineProps<{ source: LoadedTrace; row: McRecord }>();
const emit = defineEmits<{ inspect: [ref: LineRef] }>();
const expanded = ref(false);
const { value, error } = useRecord(() => props.source, () => expanded.value ? props.row.ref : undefined);
const body = computed(() => decodedObject(value.value?.data?.body) ?? value.value?.data);
const action = computed(() => object(body.value?.action) ?? body.value);
const result = computed(() => object(action.value?.result) ?? object(body.value?.result));
const nodes = computed(() => objects(object(body.value?.plan)?.nodes ?? body.value?.nodes));
const labels: Readonly<Record<string, string>> = {
  event: '游戏输入', turn: '伙伴回合', motion: '动作回执', action: '动作回执', plan: '计划',
  queue: '队列', attention: '跟随现场', question: '追问', approval: '审批', delegation: '委托',
  control: '控制状态', game: '游戏桥接',
};

// 展开时才读取该项证据，避免一次装入所有原始样本。
function toggle(event: Event) {
  expanded.value = (event.currentTarget as HTMLDetailsElement).open;
}
</script>

<template>
  <details class="receipt-card" :data-receipt="row.work ?? row.follow ?? row.plan" @toggle="toggle">
    <summary>
      <span class="receipt-summary">
        <span class="receipt-headline">
          <strong>{{ row.skill ?? labels[row.kind] }}</strong>
          <small v-if="row.work || row.follow || row.plan" class="mono">{{ row.work ?? row.follow ?? '计划 ' + row.plan }}</small>
          <small>{{ row.ref.stream === 'game' ? '游戏桥接' : '宿主记录' }}</small>
        </span>
        <span v-if="row.summary" class="receipt-excerpt">{{ row.summary }}</span>
        <span v-else-if="row.changes !== undefined || row.reason" class="receipt-excerpt">
          <span v-if="row.changes !== undefined">{{ row.changes }} 格变化</span>
          <span v-if="row.reason">{{ row.reason }}</span>
        </span>
      </span>
      <span v-if="row.delivery || row.status" class="pill" :data-tone="tone(row.delivery ?? row.status)">
        {{ statusLabel(row.delivery ?? row.status ?? '') }}
      </span>
    </summary>
    <div v-if="expanded" class="receipt-body">
      <SourceButton :address="row.ref" @inspect="emit('inspect', $event)" />
      <p v-if="error" class="notice" role="alert">{{ error }}</p>
      <dl class="data-grid compact-grid">
        <div v-if="row.effect"><dt>效果</dt><dd>{{ row.effect }}</dd></div>
        <div v-if="row.delivery"><dt>送达</dt><dd>{{ row.delivery }}</dd></div>
        <div v-if="row.status"><dt>原始状态</dt><dd>{{ row.status }}</dd></div>
        <div v-if="row.reason"><dt>依据</dt><dd>{{ row.reason }}</dd></div>
        <div v-if="row.order"><dt>事件次序</dt><dd>{{ row.order }}</dd></div>
        <div v-if="row.sourceOrder"><dt>原事件</dt><dd>{{ row.sourceOrder }}</dd></div>
        <div v-if="row.answerOrder"><dt>回答事件</dt><dd>{{ row.answerOrder }}</dd></div>
        <div v-if="row.item || row.ask"><dt>追问编号</dt><dd>{{ row.item ?? row.ask }}</dd></div>
        <div v-if="row.ref.stream === 'game' && value"><dt>桥接退出</dt><dd>{{ value.data?.exited === true ? '已确认' : '未确认' }}</dd></div>
      </dl>
      <p v-if="result?.text" class="chat-text">{{ result.text }}</p>
      <DataBlock v-if="body?.question" label="原追问" :value="body.question" />
      <DataBlock v-if="body?.answer" label="回答" :value="body.answer" />
      <section v-if="nodes.length" class="detail-section">
        <h4>计划节点 · {{ nodes.length }}</h4>
        <table class="compact-table">
          <thead><tr><th scope="col">节点</th><th scope="col">动作</th><th scope="col">状态</th></tr></thead>
          <tbody><tr v-for="(node, idx) in nodes.slice(0, 60)" :key="idx">
            <td>{{ node.id ?? '—' }}<small v-if="node.work" class="table-meta">{{ node.work }}</small></td>
            <td>{{ node.skill ?? '—' }}</td><td>{{ node.status ?? object(node.result)?.status ?? '—' }}</td>
          </tr></tbody>
        </table>
        <p v-if="nodes.length > 60" class="muted">其余节点见完整回执。</p>
      </section>
      <WorldResult v-if="result" :result="result" />
      <DataBlock v-if="result?.navigation || result?.progress || body?.progress" label="移动与跟随进度"
        :value="result?.progress ?? body?.progress ?? result?.navigation" />
      <DataBlock v-if="body" label="完整回执内容" :value="body" />
    </div>
  </details>
</template>
