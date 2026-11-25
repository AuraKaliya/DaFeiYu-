<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import EvidenceRecord from './EvidenceRecord.vue';
import type { RoundEvidence, RoundLink } from './mc-links.ts';
import type { McRecord } from './mc-records.ts';
import type { LineRef } from './trace-file.ts';
import type { LoadedTrace } from './viewer-state.ts';

const props = defineProps<{ source: LoadedTrace; evidence?: RoundEvidence }>();
const emit = defineEmits<{ inspect: [ref: LineRef]; follow: [link: RoundLink] }>();
const filter = ref('');
const limit = ref(40);
const categories = ['现场观测', '计划与动作', '追问与审批', '聊天送达', '输入与调度'];

// 依据原生动作和回执类型提供业务证据筛选。
function group(row: McRecord) {
  if (row.skill === 'observe') return '现场观测';
  if (row.kind === 'question' || row.kind === 'approval') return '追问与审批';
  if (row.skill === 'reply' || row.delivery) return '聊天送达';
  if (['motion', 'action', 'plan', 'attention', 'game'].includes(row.kind)) return '计划与动作';
  return '输入与调度';
}

const entries = computed(() => (props.evidence?.entries ?? []).filter(row => !filter.value || group(row) === filter.value));
const referenced = computed(() => (props.evidence?.references ?? []).filter(row => !filter.value || group(row) === filter.value));

// 变动后从新筛选范围的首批记录开始。
watch(filter, () => { limit.value = 40; });
</script>

<template>
  <section class="detail-stack" aria-label="MC 实效">
    <div class="section-heading">
      <h3>MC 实效</h3>
      <select v-model="filter" aria-label="领域证据类型">
        <option value="">全部领域证据</option>
        <option v-for="category in categories" :key="category">{{ category }}</option>
      </select>
    </div>
    <div v-if="evidence?.links.length" class="link-group" aria-label="关联回合">
      <button v-for="link in evidence.links" :key="link.round + link.label" type="button" class="chip"
        @click="emit('follow', link)">{{ link.label }} · {{ link.round }}</button>
    </div>
    <p v-if="!entries.length && !referenced.length" class="empty">此回合没有符合条件的领域回执。</p>

    <section v-if="entries.length" class="detail-section">
      <h4>本轮原始证据 · {{ entries.length }}</h4>
      <EvidenceRecord v-for="row in entries.slice(0, limit)" :key="row.ref.stream + row.ref.line"
        :source="source" :row="row" @inspect="emit('inspect', $event)" />
    </section>
    <section v-if="referenced.length" class="detail-section">
      <h4>原回合证据 · {{ referenced.length }}</h4>
      <EvidenceRecord v-for="row in referenced.slice(0, limit)" :key="row.ref.stream + row.ref.line"
        :source="source" :row="row" @inspect="emit('inspect', $event)" />
    </section>
    <button v-if="entries.length > limit || referenced.length > limit" class="button subtle" type="button"
      @click="limit += 40">显示更多回执</button>
  </section>
</template>
