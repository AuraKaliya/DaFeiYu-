<script setup lang="ts">
import { computed, ref } from 'vue';
import DataBlock from './DataBlock.vue';
import SourceButton from './SourceButton.vue';
import { comparePrompts, systemParts } from './model-stages.ts';
import { mcInput } from './rounds.ts';
import { object, objects, text } from './trace-file.ts';
import { useRecord } from './use-record.ts';
import type { RecordEntry } from './rounds.ts';
import type { LineRef } from './trace-file.ts';
import type { LoadedTrace } from './viewer-state.ts';

const props = defineProps<{ source: LoadedTrace; prepared: RecordEntry; previous?: RecordEntry }>();
const emit = defineEmits<{ inspect: [ref: LineRef] }>();
const { value, error, pending } = useRecord(() => props.source, () => props.prepared.ref);
const compare = ref(false);
const { value: earlierRecord, error: previousError } = useRecord(() => props.source, () => compare.value ? props.previous?.ref : undefined);
const request = computed(() => object(value.value?.data?.input));
const messages = computed(() => objects(request.value?.messages));
const tools = computed(() => objects(request.value?.tools));
const facts = computed(() => mcInput(request.value));
const systems = computed(() => messages.value.flatMap(message => {
  const parts = message.role === 'system' ? systemParts(message.content) : undefined;
  return parts ? [parts] : [];
}));
const difference = computed(() => {
  const earlier = object(earlierRecord.value?.data?.input);
  const current = request.value;
  if (earlier && current) return comparePrompts(earlier, current);
});
const factNames: Readonly<Record<string, string>> = {
  events: '本轮游戏输入', currentEvents: '原游戏事件', newEvents: '新增游戏事件', lastObservation: '最近观测',
  sceneRevision: '现场修订', task: '任务状态', taskPlan: '当前计划', taskHandoff: '任务交接',
  planSettlements: '计划结算', workSettlement: '动作回执', work: '复核中的工作', following: '跟随状态',
  activeMotion: '在途动作', queuedActions: '已排队动作', planRejection: '本次计划拒绝', previousPlanRejection: '前次计划拒绝',
  followAttention: '跟随现场', recentDialogue: '最近对话', pendingQuestion: '待答事项', questionResult: '追问结果',
  pendingApproval: '待审批事项', approvalResult: '审批结果', delegation: '主 Agent 委托', eventGap: '事件缺口',
};
const groups = computed(() => Object.entries(facts.value ?? {}).filter(([key]) => Object.hasOwn(factNames, key)));
const messageChanges = { added: '新增', removed: '移除', changed: '内容变化' };
</script>

<template>
  <section class="detail-stack" aria-label="实际送模">
    <div class="section-heading">
      <h3>实际送模</h3>
      <SourceButton :address="prepared.ref" @inspect="emit('inspect', $event)" />
    </div>
    <p v-if="error" class="notice" role="alert">{{ error }}</p>
    <p v-else-if="pending" class="empty" aria-live="polite">正在读取送模原文…</p>
    <p v-else-if="!request" class="empty">这条装配记录缺少可读取的模型请求。</p>
    <template v-else>
      <dl class="data-grid">
        <div><dt>模型</dt><dd>{{ value?.data?.model ?? '—' }}</dd></div>
        <div><dt>提供方</dt><dd>{{ value?.data?.provider ?? '—' }}</dd></div>
        <div><dt>人格版本</dt><dd>{{ value?.data?.personaProfile ?? '—' }}</dd></div>
        <div><dt>输出预算</dt><dd>{{ request.maxOutputTokens ?? '—' }}</dd></div>
        <div v-if="typeof value?.data?.thinking === 'boolean'"><dt>思考模式</dt><dd>{{ value.data.thinking ? '开启' : '关闭' }}</dd></div>
      </dl>
      <p v-if="value?.data?.promptTemplate" class="fine-print mono">模板 {{ value.data.promptTemplate }}</p>

      <section v-if="previous" class="detail-section">
        <button type="button" class="text-button" :aria-expanded="compare" @click="compare = !compare">对照上一次装配</button>
        <p v-if="compare && previousError" class="notice">{{ previousError }}</p>
        <div v-else-if="compare && difference" class="comparison">
          <dl class="data-grid">
            <div><dt>新增工具</dt><dd>{{ difference.added.join('、') || '无' }}</dd></div>
            <div><dt>移除工具</dt><dd>{{ difference.removed.join('、') || '无' }}</dd></div>
            <div><dt>工具契约变化</dt><dd>{{ difference.changed.join('、') || '无' }}</dd></div>
            <div><dt>MC 事实字段变化</dt><dd>{{ difference.facts.join('、') || '无' }}</dd></div>
          </dl>
          <p v-if="!difference.messages.length" class="muted">消息顺序和正文一致。</p>
          <ul v-else class="link-group" aria-label="消息位置变化">
            <li v-for="message in difference.messages" :key="message.position" class="pill">
              {{ message.position }}. {{ message.role }} · {{ messageChanges[message.change] }}
            </li>
          </ul>
          <SourceButton v-if="previous" :address="previous.ref" @inspect="emit('inspect', $event)" />
        </div>
        <p v-else-if="compare" class="empty">正在读取前一次装配…</p>
      </section>

      <section v-if="facts" class="detail-section">
        <h4>MC 上下文</h4>
        <dl class="data-grid">
          <div v-if="facts.player"><dt>玩家</dt><dd>{{ facts.player }}</dd></div>
          <div v-if="facts.generation"><dt>世界代际</dt><dd>{{ facts.generation }}</dd></div>
          <div v-if="facts.foreground"><dt>当前阶段</dt><dd>{{ facts.foreground }}</dd></div>
        </dl>
        <DataBlock v-for="[key, item] in groups" :key="key" :label="factNames[key]" :value="item" />
      </section>

      <section v-if="systems.length" class="detail-section">
        <h4>人格与领域指引</h4>
        <template v-for="(parts, idx) in systems" :key="idx">
          <DataBlock v-if="parts.persona" label="人格正文" :value="parts.persona" />
          <DataBlock :label="parts.title" :value="parts.guidance" />
        </template>
      </section>

      <section class="detail-section">
        <h4>模型消息 · {{ messages.length }}</h4>
        <div v-for="(message, idx) in messages" :key="idx" class="message-block">
          <DataBlock :label="`${idx + 1}. ${text(message.role) ?? '未知角色'}${message.toolCallId ? ' · ' + message.toolCallId : ''}`"
            :value="message.content ?? '—'" :open="idx === 0" />
          <DataBlock v-if="objects(message.toolCalls).length" label="消息附带的工具调用" :value="message.toolCalls" />
          <DataBlock v-if="message.reasoningContent !== undefined" label="消息附带的模型续行内容" :value="message.reasoningContent" />
        </div>
      </section>

      <section class="detail-section">
        <h4>本次工具契约 · {{ tools.length }}</h4>
        <DataBlock v-for="(tool, idx) in tools" :key="idx" :label="text(tool.name) ?? '未命名工具'" :value="tool" />
        <p v-if="!tools.length" class="muted">没有随此请求提供的工具定义。</p>
      </section>
    </template>
  </section>
</template>
