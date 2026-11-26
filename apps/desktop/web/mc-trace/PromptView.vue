<script setup lang="ts">
import { computed, ref, useId } from 'vue';
import DataBlock from './DataBlock.vue';
import SourceButton from './SourceButton.vue';
import { comparePrompts, systemParts } from './model-stages.ts';
import { preparedMessages, type PreparedMessage } from './prepared-messages.ts';
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
const messages = computed(() => preparedMessages(value.value?.data));
const tail = computed(() => messages.value.at(-1));
const statusHeading = useId();
const tools = computed(() => objects(request.value?.tools));
const facts = computed(() => mcInput(request.value));
const systems = computed(() => messages.value.flatMap(({ message }) => {
  const parts = message.role === 'system' ? systemParts(message.content) : undefined;
  return parts ? [parts] : [];
}));
const difference = computed(() => {
  const earlier = object(earlierRecord.value?.data?.input);
  const current = request.value;
  if (earlier && current) return comparePrompts(earlier, current);
});
const factNames: Readonly<Record<string, string>> = {
  mcGuidance: '本轮 MC 指引',
  events: '本轮游戏输入', currentEvents: '原游戏事件', newEvents: '新增游戏事件', lastObservation: '最近观测',
  sceneRevision: '现场修订', task: '任务状态', taskPlan: '当前计划', taskHandoff: '任务交接',
  planSettlements: '计划结算', workSettlement: '动作回执', work: '复核中的工作', following: '跟随状态',
  activeMotion: '在途动作', queuedActions: '已排队动作', planRejection: '本次计划拒绝', previousPlanRejection: '前次计划拒绝',
  followAttention: '跟随现场', recentDialogue: '最近对话', pendingQuestion: '待答事项', questionResult: '追问结果',
  pendingApproval: '待审批事项', approvalResult: '审批结果', delegation: '主 Agent 委托', eventGap: '事件缺口',
};
const groups = computed(() => Object.entries(facts.value ?? {}).filter(([key]) => Object.hasOwn(factNames, key)));
const messageChanges = { added: '新增', removed: '移除', changed: '内容变化' };

// 用原位置、协议角色和已记录的来源标识正文，旧日志沿用原角色。
function messageLabel(entry: PreparedMessage): string {
  const parts = [`${entry.position}. ${text(entry.message.role) ?? '未知角色'}`];
  const callId = text(entry.message.toolCallId);
  if (callId) parts.push(callId);
  if (entry.source) parts.push(`来源 ${entry.source}`);
  if (entry.eventUtcMs !== undefined) parts.push(`UTC 毫秒 ${entry.eventUtcMs}`);
  if (entry.status) parts.push('宿主快照原文');
  return parts.join(' · ');
}
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

      <section v-if="tail?.status" class="detail-section" :aria-labelledby="statusHeading">
        <div class="section-heading">
          <h4 :id="statusHeading">本步宿主快照</h4>
          <SourceButton :address="prepared.ref" @inspect="emit('inspect', $event)" />
        </div>
        <dl class="data-grid compact-grid">
          <div><dt>快照号</dt><dd>{{ tail.status.snapshot }}</dd></div>
          <div><dt>送模原记录</dt><dd>步骤 {{ prepared.step }} · 尝试 {{ prepared.attempt }} · 消息 {{ tail.position }}</dd></div>
          <div><dt>会话 / 代次</dt><dd>{{ tail.status.scope.session }} / {{ tail.status.scope.epoch }}</dd></div>
          <div><dt>尾注回合 / 步骤</dt><dd>{{ tail.status.turn }} / {{ tail.status.step }}</dd></div>
          <div v-if="tail.status.scope.task !== undefined"><dt>任务</dt><dd>{{ tail.status.scope.task }}</dd></div>
          <div><dt>消息来源</dt><dd>{{ tail.source }}</dd></div>
          <div><dt>状态项</dt><dd>{{ tail.status.items.length }}</dd></div>
        </dl>
        <DataBlock label="判定截止（asOf）" :value="tail.status.asOf" />
        <DataBlock :label="`唤醒事件 · ${tail.status.wakeEvents.length}`" :value="tail.status.wakeEvents" />
        <DataBlock v-for="(item, idx) in tail.status.items" :key="idx"
          :label="`${item.key} · 来源 ${item.source}`" :value="item" />
      </section>

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
        <div v-for="entry in messages" :key="entry.position" class="message-block">
          <DataBlock :label="messageLabel(entry)" :value="entry.message.content ?? '—'" :open="entry.position === 1" />
          <DataBlock v-if="objects(entry.message.toolCalls).length" label="消息附带的工具调用" :value="entry.message.toolCalls" />
          <DataBlock v-if="entry.message.reasoningContent !== undefined" label="消息附带的模型续行内容" :value="entry.message.reasoningContent" />
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
