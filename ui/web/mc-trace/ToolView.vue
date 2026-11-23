<script setup lang="ts">
import { ref } from 'vue';
import DataBlock from './DataBlock.vue';
import SourceButton from './SourceButton.vue';
import { phaseLabel, statusLabel, tone } from './display.ts';
import { useRecord } from './use-record.ts';
import type { ToolCall } from './model-stages.ts';
import type { LineRef } from './trace-file.ts';
import type { LoadedTrace } from './viewer-state.ts';

const props = defineProps<{ source: LoadedTrace; tool: ToolCall }>();
const emit = defineEmits<{ inspect: [ref: LineRef] }>();
const expanded = ref(false);
const { value: args, error: argsError } = useRecord(() => props.source,
  () => expanded.value ? (props.tool.admitted ?? props.tool.records[0]).ref : undefined);
const { value: outcome, error: resultError } = useRecord(() => props.source,
  () => expanded.value ? props.tool.exited?.ref : undefined);
const observed = props.tool.exited?.business ?? props.tool.exited?.result;
const admission = props.tool.admitted ? '已接纳' : props.tool.exited?.accepted === false ? '未接纳' : '未见接纳记录';

// 读回展开的候选参数和工具结果，保持已显示的裁定摘要。
function toggle(event: Event) {
  expanded.value = (event.currentTarget as HTMLDetailsElement).open;
}
</script>

<template>
  <details class="tool-card" :data-tool="tool.key" @toggle="toggle">
    <summary>
      <code>{{ tool.name }}</code>
      <span class="pill" :data-tone="tone(observed)">
        {{ tool.exited?.accepted === false && !tool.admitted ? '未接纳' : observed ? statusLabel(observed) : admission }}
      </span>
    </summary>
    <div v-if="expanded" class="tool-body">
      <dl class="data-grid">
        <div><dt>候选号</dt><dd>{{ tool.key }}</dd></div>
        <div><dt>模型调用号</dt><dd>{{ tool.callId ?? '—' }}</dd></div>
        <div><dt>宿主接纳</dt><dd>{{ admission }}</dd></div>
        <div v-if="tool.admitted?.invocation"><dt>宿主调用号</dt><dd>{{ tool.admitted.invocation }}</dd></div>
        <div v-if="tool.exited?.confirmation || tool.admitted?.confirmation">
          <dt>宿主审批号</dt><dd>{{ tool.exited?.confirmation ?? tool.admitted?.confirmation }}</dd>
        </div>
        <div><dt>宿主退出</dt><dd>{{ tool.exited?.result ?? '未见退出' }}</dd></div>
        <div v-if="tool.exited?.business"><dt>领域状态</dt><dd>{{ tool.exited.business }}</dd></div>
        <div v-if="tool.exited?.reason"><dt>依据</dt><dd>{{ tool.exited.reason }}</dd></div>
        <div v-if="tool.exited?.effect"><dt>效果标记</dt><dd>{{ tool.exited.effect }}</dd></div>
        <div v-if="tool.exited?.fault && tool.exited.fault !== 'none'"><dt>故障类目</dt><dd>{{ tool.exited.fault }}</dd></div>
      </dl>
      <div class="source-stack">
        <div v-for="record in tool.records" :key="record.ref.line">
          <small>{{ phaseLabel(record.kind) }}</small>
          <SourceButton :address="record.ref" @inspect="emit('inspect', $event)" />
        </div>
      </div>
      <p v-if="argsError || resultError" class="notice">{{ argsError || resultError }}</p>
      <DataBlock v-if="args?.data?.arguments !== undefined" label="调用参数" :value="args?.data?.arguments" />
      <DataBlock v-if="outcome?.data?.output !== undefined" label="工具实际返回" :value="outcome?.data?.output" />
    </div>
  </details>
</template>
