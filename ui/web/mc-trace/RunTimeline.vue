<script setup lang="ts">
import { computed } from 'vue';
import { duration, phaseLabel, roundStatus, statusLabel, tone } from './display.ts';
import { milliseconds, stageKey, stageTiming } from './model-stages.ts';
import type { ModelStage } from './model-stages.ts';
import type { Round, RecordEntry } from './rounds.ts';
import type { LineRef } from './trace-file.ts';

const props = defineProps<{ round: Round; stages: readonly ModelStage[]; active: string; evidenceCount: number }>();
const emit = defineEmits<{ stage: [key: string, outcome?: boolean]; record: [ref: LineRef]; mc: [] }>();
const start = computed(() => props.round.events[0]?.time);
const total = computed(() => milliseconds(start.value, props.round.exit?.time));
const nodes = computed(() => {
  const firsts = new Map(props.stages.map(stage => [stage.records[0], stage]));
  return props.round.events.flatMap<{ record: RecordEntry; stage?: ModelStage }>(record => {
    const stage = firsts.get(record);
    if (stage) return [{ record, stage }];
    if (!stageKey(record)) return [{ record, stage: undefined }];
    return [];
  });
});

// 标记一次实际调用的退出或尚缺的证据。
function modelStatus(stage: ModelStage) {
  if (stage.exited?.result) return statusLabel(stage.exited.result);
  return stage.submitted ? '未见响应' : '未见提交';
}

// 为附带工具标注领域结果或宿主阶段。
function toolStatus(entry?: RecordEntry) {
  const result = entry?.business ?? entry?.result;
  return result ? statusLabel(result) : '未见退出';
}
</script>

<template>
  <main class="round" aria-label="回合阶段">
    <header class="round-head">
      <div class="run-context">
        <span>{{ round.input?.trigger.category ?? '触发待确认' }}</span>
        <code>会话 {{ round.session }} / 回合 {{ round.run }}</code>
      </div>
      <h2>{{ round.input?.trigger.label ?? '触发待确认' }}</h2>
      <p v-if="round.input?.trigger.code" class="trigger-code mono">{{ round.input.trigger.code }}</p>
      <ul v-if="round.input?.facts.some(fact => fact.text)" class="trigger-facts">
        <li v-for="(fact, index) in round.input.facts.filter(row => row.text)" :key="index">
          <span>{{ fact.text }}</span><small v-if="fact.sequence">事件 {{ fact.sequence }}</small>
        </li>
      </ul>
      <p v-if="round.input?.reason" class="muted">触发依据 <code>{{ round.input.reason }}</code></p>
      <dl class="round-metrics">
        <div><dt>宿主回合</dt><dd>{{ roundStatus(round) }}</dd></div>
        <div><dt>模型调用</dt><dd>{{ round.modelCount }}</dd></div>
        <div><dt>已记录跨度</dt><dd>{{ duration(total) }}</dd></div>
      </dl>
    </header>
    <section class="phase-lane">
      <div class="section-heading">
        <h3>阶段进展</h3>
        <button class="text-button" type="button" @click="emit('mc')">MC 实效 · {{ evidenceCount }}</button>
      </div>
      <ol class="phase-list">
        <li v-for="{ record, stage } in nodes" :key="record.ref.line" :class="{ 'has-model': stage }">
          <article v-if="stage" class="phase-card" :class="{ 'is-active': stage.key === active }">
            <button class="phase-main" type="button" :aria-pressed="stage.key === active"
              :data-stage="stage.key" @click="emit('stage', stage.key)">
              <span><strong>模型步骤 {{ stage.step }}</strong><small>第 {{ stage.attempt }} 次尝试</small></span>
              <span class="phase-duration mono">{{ duration(stageTiming(stage).service) }}<small>{{ modelStatus(stage) }}</small></span>
            </button>
            <div class="phase-chips" aria-label="模型生命周期">
              <button v-for="phase in [stage.prepared, stage.submitted, stage.exited, stage.rewritten].filter(row => row !== undefined)"
                :key="phase.ref.line" type="button" class="chip" :title="`查看原记录第 ${phase.ref.line} 行`"
                @click="emit('record', phase.ref)">{{ phaseLabel(phase.kind) }}</button>
            </div>
            <button v-for="tool in stage.tools" :key="tool.key" class="tool-preview" type="button" @click="emit('stage', stage.key, true)">
              <code>{{ tool.name }}</code>
              <span class="tone-text" :data-tone="tone(tool.exited?.business ?? tool.exited?.result)">{{ toolStatus(tool.exited) }}</span>
            </button>
          </article>
          <button v-else type="button" class="phase-event" @click="emit('record', record.ref)">
            <span>{{ phaseLabel(record.kind) }} <code v-if="record.result">{{ record.result }}</code></span>
            <small class="mono">+{{ duration(milliseconds(start, record.time)) }}</small>
          </button>
        </li>
      </ol>
    </section>
  </main>
</template>
