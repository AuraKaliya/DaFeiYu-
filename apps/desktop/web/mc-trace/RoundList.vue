<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { roundStatus, tone } from './display.ts';
import type { Round } from './rounds.ts';

const props = defineProps<{ rounds: readonly Round[]; selected: string }>();
const emit = defineEmits<{ select: [key: string]; raw: [] }>();
const query = ref('');
const category = ref('');
const status = ref('');
const limit = ref(60);
const facets = computed(() => [...new Set(props.rounds.map(round => round.input?.trigger.category ?? '触发待确认'))]);
const statuses = computed(() => [...new Set(props.rounds.map(roundStatus))]);

// 在已确认的触发、身份和玩家原话中筛选回合。
const matching = computed(() => {
  const needle = query.value.trim().toLowerCase();
  return props.rounds.filter(round => {
    const trigger = round.input?.trigger;
    if (category.value && category.value !== (trigger?.category ?? '触发待确认')) return false;
    if (status.value && status.value !== roundStatus(round)) return false;
    const keywords = [round.key, trigger?.label, trigger?.code, round.input?.generation,
      ...round.input?.facts.map(fact => fact.text) ?? []];
    return !needle || keywords.some(value => value?.toLowerCase().includes(needle));
  });
});

// 筛选改变时复位列表上限。
watch([query, category, status], () => { limit.value = 60; });
</script>

<template>
  <aside class="navigator" aria-label="MC 回合">
    <div class="navigator-header">
      <h2>Agent 回合 <span class="count">{{ matching.length }}</span></h2>
      <button class="text-button" type="button" @click="emit('raw')">全局日志</button>
    </div>
    <div v-if="rounds.length" class="filters">
      <input v-model="query" type="search" placeholder="搜索原话、触发或回合号" aria-label="搜索回合">
      <div class="filter-row">
        <select v-model="category" aria-label="触发分类">
          <option value="">全部触发</option>
          <option v-for="facet in facets" :key="facet">{{ facet }}</option>
        </select>
        <select v-model="status" aria-label="回合状态">
          <option value="">全部状态</option>
          <option v-for="entry in statuses" :key="entry">{{ entry }}</option>
        </select>
      </div>
    </div>
    <ol class="round-list">
      <li v-for="round in matching.slice(0, limit)" :key="round.key">
        <button type="button" class="round-item" :data-round="round.key"
          :aria-current="round.key === selected ? 'true' : undefined" @click="emit('select', round.key)">
          <span class="list-row">
            <span class="round-title">{{ round.input?.trigger.label ?? '触发待确认' }}</span>
            <span class="round-key mono">{{ round.key }}</span>
          </span>
          <span class="round-excerpt">{{ round.input?.facts[0]?.text || round.input?.reason || round.input?.trigger.code || '无可读触发正文' }}</span>
          <span class="list-row round-meta">
            <span class="tone-text" :data-tone="round.exit ? tone(round.exit.result) : 'pending'">{{ roundStatus(round) }}</span>
            <span>{{ round.modelCount }} 次调用</span>
          </span>
        </button>
      </li>
    </ol>
    <p v-if="rounds.length && !matching.length" class="empty">没有匹配的回合。</p>
    <button v-if="matching.length > limit" class="button subtle list-more" type="button" @click="limit += 60">显示更多回合</button>
  </aside>
</template>
