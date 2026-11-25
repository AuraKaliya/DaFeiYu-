<script setup lang="ts">
import { computed, ref } from 'vue';
import DataBlock from './DataBlock.vue';
import { coordinates } from './display.ts';
import { object, objects } from './trace-file.ts';
import type { Data } from './trace-file.ts';

const props = defineProps<{ result: Data }>();
const sampleLimit = ref(10);
const observation = computed(() => object(props.result.observation));
const samples = computed(() => objects(observation.value?.samples));
const sources: Readonly<Record<string, string>> = { self: '伙伴自身', player: '玩家当下', message: '消息发生时' };

// 解释本次采样的视角来源，保留未来或未知来源的原生码。
function viewLabel(source: unknown) {
  if (typeof source !== 'string') return '来源待确认';
  return Object.hasOwn(sources, source) ? sources[source] : source;
}
</script>

<template>
  <section v-if="observation || result.sampledAtMs !== undefined" class="detail-section">
    <h4>观测取证</h4>
    <dl v-if="observation" class="data-grid">
      <div><dt>采样编号</dt><dd>{{ observation.id ?? '—' }}</dd></div>
      <div><dt>维度</dt><dd>{{ observation.dimension ?? '—' }}</dd></div>
      <div><dt>请求采样</dt><dd>{{ observation.requested ?? '—' }}</dd></div>
      <div><dt>实得样本</dt><dd>{{ samples.length }}</dd></div>
    </dl>
    <article v-for="(sample, index) in samples.slice(0, sampleLimit)" :key="index" class="sample">
      <div class="section-heading">
        <strong>第 {{ index + 1 }} 次采样</strong>
        <small>游戏时钟 {{ sample.sampledAtMs ?? '—' }} ms</small>
      </div>
      <dl v-if="object(sample.coverage)" class="data-grid compact-grid">
        <div><dt>覆盖格数</dt><dd>{{ object(sample.coverage)?.unique ?? '—' }}</dd></div>
        <div><dt>未知格数</dt><dd>{{ object(sample.coverage)?.unknown ?? '—' }}</dd></div>
      </dl>
      <div v-for="(view, viewIndex) in objects(sample.views)" :key="viewIndex" class="sample-view">
        <div class="section-heading">
          <strong>{{ viewLabel(view.source) }}<span v-if="view.message"> · 事件 {{ view.message }}</span></strong>
          <code>{{ view.status ?? '—' }}</code>
        </div>
        <dl class="data-grid compact-grid">
          <div v-if="object(view.actor)?.position || object(view.anchor)?.position">
            <dt>位置</dt><dd>{{ coordinates(object(view.actor)?.position ?? object(view.anchor)?.position) }}</dd>
          </div>
          <div v-if="view.belowFeet"><dt>脚下方块</dt><dd>{{ object(view.belowFeet)?.block ?? '—' }} · {{ coordinates(view.belowFeet) }}</dd></div>
          <div v-if="object(view.coverage)"><dt>覆盖尺寸</dt>
            <dd>{{ object(view.coverage)?.width ?? '—' }} × {{ object(view.coverage)?.height ?? '—' }} × {{ object(view.coverage)?.depth ?? '—' }}</dd></div>
          <div v-if="object(view.anchor)?.sampledAtMs !== undefined"><dt>消息时钟</dt><dd>{{ object(view.anchor)?.sampledAtMs }} ms</dd></div>
        </dl>
        <DataBlock v-if="view.playerFocus" label="命中与候选目标" :value="view.playerFocus" />
        <DataBlock v-if="object(view.actor)?.inventory" label="观测背包" :value="object(view.actor)?.inventory" />
      </div>
      <DataBlock v-if="sample.verified !== undefined" label="主动核对的方格" :value="sample.verified" />
    </article>
    <button v-if="samples.length > sampleLimit" class="text-button" type="button" @click="sampleLimit += 10">显示更多采样</button>

    <template v-if="!observation">
      <dl class="data-grid">
        <div><dt>游戏时钟</dt><dd>{{ result.sampledAtMs }} ms</dd></div>
        <div><dt>维度</dt><dd>{{ result.dimension ?? '—' }}</dd></div>
        <div><dt>伙伴位置</dt><dd>{{ coordinates(result.position) }}</dd></div>
        <div><dt>玩家位置</dt><dd>{{ coordinates(result.player) }}</dd></div>
      </dl>
      <DataBlock v-if="result.playerFocus" label="消息视角与目标候选" :value="result.playerFocus" />
      <DataBlock v-if="result.inventory" label="观测背包" :value="result.inventory" />
    </template>
  </section>
</template>
