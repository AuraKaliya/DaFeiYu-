<script setup lang="ts">
import { computed, ref } from 'vue';

const props = defineProps<{ label: string; value: unknown; open?: boolean }>();
const expanded = ref(Boolean(props.open));
const full = ref(false);
const previewSize = 24 * 1024;
const content = computed(() => typeof props.value === 'string' ? props.value : JSON.stringify(props.value, null, 2) ?? '—');

// 同步原生折叠状态，关闭时释放详情的预览区域。
function toggle(event: Event) {
  expanded.value = (event.currentTarget as HTMLDetailsElement).open;
  if (!expanded.value) full.value = false;
}
</script>

<template>
  <details class="data-block" :open="expanded" @toggle="toggle">
    <summary>{{ label }}</summary>
    <div v-if="expanded" class="data-body">
      <pre class="code-block" tabindex="0">{{ full ? content : content.slice(0, previewSize) }}</pre>
      <button v-if="content.length > previewSize && !full" type="button" class="text-button" @click="full = true">展开完整内容</button>
    </div>
  </details>
</template>
