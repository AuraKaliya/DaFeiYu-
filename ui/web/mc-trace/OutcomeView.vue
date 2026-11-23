<script setup lang="ts">
import { computed } from 'vue';
import DataBlock from './DataBlock.vue';
import SourceButton from './SourceButton.vue';
import ToolView from './ToolView.vue';
import { duration } from './display.ts';
import { policyFacts, stageTiming } from './model-stages.ts';
import { object, objects } from './trace-file.ts';
import { useRecord } from './use-record.ts';
import type { ModelStage } from './model-stages.ts';
import type { Round } from './rounds.ts';
import type { LineRef } from './trace-file.ts';
import type { LoadedTrace } from './viewer-state.ts';

const props = defineProps<{ source: LoadedTrace; round: Round; stage?: ModelStage }>();
const emit = defineEmits<{ inspect: [ref: LineRef] }>();
const { value: model, error: modelError, pending: modelPending } = useRecord(() => props.source, () => props.stage?.exited?.ref);
const { value: rewrite, error: rewriteError } = useRecord(() => props.source, () => props.stage?.rewritten?.ref);
const { value: exit, error: exitError } = useRecord(() => props.source, () => props.round.exit?.ref);
const timing = computed(() => props.stage ? stageTiming(props.stage) : undefined);
const output = computed(() => object(model.value?.data?.output));
const message = computed(() => object(output.value?.message));
const usage = computed(() => object(output.value?.usage));
const calls = computed(() => objects(message.value?.toolCalls));
const policy = computed(() => policyFacts(exit.value?.data, props.stage));
</script>

<template>
  <section class="detail-stack" aria-label="响应与裁定">
    <div v-if="stage" class="detail-section">
      <h3>模型耗时</h3>
      <dl class="data-grid timing-grid">
        <div><dt>提交到开始</dt><dd>{{ duration(timing?.queue) }}</dd></div>
        <div><dt>模型响应</dt><dd>{{ duration(timing?.service) }}</dd></div>
        <div><dt>完成到宿主读回</dt><dd>{{ duration(timing?.readback) }}</dd></div>
      </dl>
    </div>

    <section v-if="stage" class="detail-section">
      <div class="section-heading">
        <h3>模型输出</h3>
        <SourceButton v-if="stage.exited" :address="stage.exited.ref" @inspect="emit('inspect', $event)" />
      </div>
      <p v-if="modelError" class="notice" role="alert">{{ modelError }}</p>
      <p v-else-if="modelPending" class="empty" aria-live="polite">正在读取模型响应…</p>
      <p v-else-if="!model" class="empty">{{ stage.submitted ? '未见响应记录。' : '未见提交记录。' }}</p>
      <template v-else>
        <dl class="data-grid">
          <div><dt>宿主记录</dt><dd>{{ model.data?.result ?? '—' }}</dd></div>
          <div><dt>结束原因</dt><dd>{{ output?.finish ?? model.data?.error ?? '—' }}</dd></div>
          <div><dt>输入 Token</dt><dd>{{ usage?.inputTokens ?? '—' }}</dd></div>
          <div><dt>输出 Token</dt><dd>{{ usage?.outputTokens ?? '—' }}</dd></div>
        </dl>
        <DataBlock v-if="message?.content" label="模型返回文本" :value="message.content" open />
        <p v-else-if="model.data?.result === 'succeeded' && !calls.length" class="muted">响应未包含文本或工具调用。</p>
        <DataBlock v-for="(call, index) in calls" :key="index" :label="`模型提出 · ${call.name ?? '未命名工具'}`" :value="call" />
      </template>
      <p v-if="rewriteError" class="notice">{{ rewriteError }}</p>
      <div v-if="stage.rewritten" class="source-row">
        <strong>宿主改写</strong><SourceButton :address="stage.rewritten.ref" @inspect="emit('inspect', $event)" />
      </div>
      <DataBlock v-if="rewrite?.data" label="改写记录" :value="rewrite.data" />
    </section>

    <section v-if="stage" class="detail-section">
      <h3>工具调用 · {{ stage.tools.length }}</h3>
      <ToolView v-for="tool in stage.tools" :key="tool.key" :source="source" :tool="tool" @inspect="emit('inspect', $event)" />
      <p v-if="!stage.tools.length" class="muted">未见该尝试的工具候选、接纳或退出记录。</p>
    </section>

    <section class="detail-section">
      <div class="section-heading">
        <h3>程序裁定与回合结束</h3>
        <SourceButton v-if="round.exit" :address="round.exit.ref" @inspect="emit('inspect', $event)" />
      </div>
      <p v-if="exitError" class="notice">{{ exitError }}</p>
      <p v-if="!round.exit" class="empty">未见该回合的退出记录。</p>
      <dl v-else class="data-grid">
        <div><dt>宿主结果</dt><dd>{{ round.exit.result ?? '已退出' }}</dd></div>
        <div><dt>停止原因</dt><dd>{{ round.exit.reason ?? '—' }}</dd></div>
      </dl>
      <DataBlock v-for="(decision, index) in policy" :key="index"
        :label="`${decision.control_point ?? '控制点'} · ${decision.decision ?? '裁定'}`" :value="decision" />
      <DataBlock v-if="exit?.data?.answer" label="回合出口文本" :value="exit.data.answer" />
    </section>
  </section>
</template>
