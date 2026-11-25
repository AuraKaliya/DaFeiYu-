<script setup lang="ts">
import { computed, ref } from 'vue';
import DataBlock from './DataBlock.vue';
import ObservationView from './ObservationView.vue';
import { coordinates } from './display.ts';
import { object, objects } from './trace-file.ts';
import type { Data } from './trace-file.ts';

const props = defineProps<{ result: Data }>();
const limit = ref(24);
const changes = computed(() => objects(props.result.changes));
const cells = computed(() => objects(props.result.cells));
const stock = computed(() => object(props.result.stock));
const craft = computed(() => object(props.result.craft));
const delta = computed(() => Object.entries(object(props.result.inventoryDelta) ?? {}));

// 标出实际背包增减，同时保留非数值的旧格式。
function signed(value: unknown) {
  if (typeof value !== 'number') return value;
  return value > 0 ? '+' + value : value;
}
</script>

<template>
  <ObservationView :result="result" />
  <section v-if="Array.isArray(result.changes)" class="detail-section">
    <h4>实际方块变化 · {{ changes.length }} 格</h4>
    <table v-if="changes.length" class="compact-table">
      <thead><tr><th scope="col">坐标</th><th scope="col">原方块</th><th scope="col">现方块</th></tr></thead>
      <tbody><tr v-for="(change, index) in changes.slice(0, limit)" :key="index">
        <td class="mono">{{ coordinates(change) }}</td><td>{{ change.before ?? '—' }}</td><td>{{ change.after ?? '—' }}</td>
      </tr></tbody>
    </table>
    <p v-else class="muted">回执记录了 0 格变化。</p>
    <button v-if="changes.length > limit" class="text-button" type="button" @click="limit += 24">显示更多变化</button>
  </section>
  <DataBlock v-if="cells.length" :label="`逐格处置 · ${cells.length} 格`" :value="cells" />

  <section v-if="stock || craft || delta.length" class="detail-section">
    <h4>{{ craft ? '合成与背包' : '备料与背包' }}</h4>
    <dl v-if="craft" class="data-grid">
      <div><dt>合成物</dt><dd>{{ result.item ?? '—' }}</dd></div>
      <div><dt>请求数量</dt><dd>{{ result.count ?? '—' }}</dd></div>
      <div><dt>已消耗 {{ craft.ingredient || '原料' }}</dt><dd>{{ craft.consumed ?? '—' }}</dd></div>
      <div><dt>已产出</dt><dd>{{ craft.produced ?? '—' }}</dd></div>
      <div><dt>仍缺</dt><dd>{{ craft.remaining ?? '—' }}</dd></div>
      <div v-if="craft.completedBatches !== undefined"><dt>已完成批次</dt><dd>{{ craft.completedBatches }} / {{ craft.batches ?? '—' }}</dd></div>
    </dl>
    <dl v-if="stock" class="data-grid">
      <div><dt>材料</dt><dd>{{ stock.item ?? '—' }}</dd></div>
      <div><dt>原有</dt><dd>{{ stock.before ?? '—' }}</dd></div>
      <div><dt>所需</dt><dd>{{ stock.required ?? '—' }}</dd></div>
      <div><dt>现有</dt><dd>{{ stock.after ?? '—' }}</dd></div>
    </dl>
    <dl v-if="delta.length" class="data-grid inventory-delta">
      <div v-for="[name, amount] in delta" :key="name"><dt>{{ name }}</dt><dd>{{ signed(amount) }}</dd></div>
    </dl>
  </section>
</template>
