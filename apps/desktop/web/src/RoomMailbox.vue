<script setup lang="ts">
import { computed } from 'vue';
import type { MailboxView } from './mailbox-view';

const props = defineProps<{ view: MailboxView }>();
const emit = defineEmits<{ visibility: [open: boolean]; select: [source: string]; retry: [] }>();
const sources = computed(() => props.view.directory?.sources ?? []);
const selected = computed(() => sources.value.find(item => item.id === props.view.source));
const directoryLabel = computed(() => {
  if (props.view.directoryStatus === 'unavailable') return '来源目录暂不可用';
  if (props.view.directoryStatus === 'stale') return '来源目录已过期';
  if (props.view.directoryStatus === 'loading') return '正在读取来源';
  return sources.value.length ? `${sources.value.length} 个来源` : '暂无信箱来源';
});
const detailLabel = computed(() => {
  if (!props.view.source) return props.view.selectionNotice ?? '';
  if (props.view.detailStatus === 'stale') return '所选来源已失效，请重新选择来源。';
  if (props.view.detailStatus === 'unavailable') return props.view.snapshot ? '读取失败，当前显示上次读到的信件。' : '暂时无法读取这个来源的信件。';
  if (props.view.detailStatus === 'loading') return '正在读取信件…';
  return props.view.snapshot?.messages.length ? '' : '这个来源暂无信件。';
});
const states: Readonly<Record<string, string>> = { pending: '待收取', received: '原接收方已取件' };
const kinds: Readonly<Record<string, string>> = { room: '本体书房', builtin: '内置来源', plugin: '插件来源' };
const localTime = new Intl.DateTimeFormat(undefined, {dateStyle:'short', timeStyle:'medium'});

// 只通知观察范围改变，系统信箱继续由本体持有。
function toggle(event: Event): void { emit('visibility', (event.currentTarget as HTMLDetailsElement).open); }
// 以来源身份选择详情，同名来源仍能分别读取。
function select(event: Event): void { emit('select', (event.currentTarget as HTMLSelectElement).value); }
</script>

<template>
  <details class="room-mailbox" :data-directory-status="view.directoryStatus" :data-detail-status="view.detailStatus" @toggle="toggle">
    <summary id="room-mailbox-entry"><strong>房间信箱</strong><span class="mail-meta">{{ directoryLabel }}</span></summary>
    <div class="mail-content">
      <p v-if="view.directory" class="mail-meta">目录修订 {{ view.directory.revision }}<template v-if="view.directory.sampledUtcMs !== undefined"> · 采样 <time :datetime="new Date(view.directory.sampledUtcMs).toISOString()">{{ localTime.format(view.directory.sampledUtcMs) }}</time></template></p>
      <label v-if="sources.length || view.source" class="mail-source">信件来源
        <select :value="view.source" @change="select">
          <option value="">选择来源</option>
          <option v-if="view.source && !selected" :value="view.source">{{ view.sourceLabel }}（已失效）</option>
          <option v-for="source in sources" :key="source.id" :value="source.id">{{ source.label }}</option>
        </select>
      </label>
      <p v-if="selected" class="mail-meta"><template v-if="selected.kind">{{ kinds[selected.kind] ?? selected.kind }} · </template>来源 {{ selected.id }}<template v-if="selected.generation"> · 代次 {{ selected.generation }}</template><template v-if="selected.plugin"> · 插件 {{ selected.plugin }}</template></p>
      <p v-if="view.source && view.detailStatus === 'loading'" class="mail-feedback">{{ detailLabel }}</p>
      <p class="mail-feedback" role="status">{{ view.detailStatus === 'loading' && view.source ? '' : detailLabel }}</p>
      <button v-if="view.directoryStatus === 'unavailable' || view.directoryStatus === 'stale' || (view.source && view.detailStatus === 'unavailable')"
              type="button" class="quiet-action" @click="emit('retry')">重新读取</button>
      <template v-if="view.snapshot && view.detailStatus !== 'stale' && view.snapshot.source === view.source">
        <p class="mail-meta" :data-mail-revision="view.snapshot.revision">来源修订 {{ view.snapshot.revision }}<template v-if="view.snapshot.sampledUtcMs !== undefined"> · 采样 <time :datetime="new Date(view.snapshot.sampledUtcMs).toISOString()">{{ localTime.format(view.snapshot.sampledUtcMs) }}</time></template></p>
        <ul v-if="view.snapshot.messages.length" :key="JSON.stringify([view.snapshot.instance, view.snapshot.source])" class="mail-letters" aria-label="信件">
          <li v-for="letter in view.snapshot.messages" :key="letter.id">
            <details :data-letter="letter.id">
              <summary><span>{{ letter.category }}</span><span class="mail-state">{{ states[letter.state] ?? letter.state }}</span></summary>
              <dl><dt>信件编号</dt><dd>{{ letter.id }}</dd>
                <template v-if="letter.summary"><dt>摘要</dt><dd>{{ letter.summary }}</dd></template>
                <template v-for="receipt in letter.receipts" :key="receipt.label"><dt>{{ receipt.label }}</dt><dd>{{ receipt.value }}</dd></template>
              </dl>
              <p v-if="!letter.receipts.length" class="mail-meta">暂无公开回执</p>
            </details>
          </li>
        </ul>
      </template>
    </div>
  </details>
</template>

<style scoped>
.room-mailbox { margin: 12px 0; padding: 10px 14px; border: 1px solid var(--line, #d7dbcf); border-radius: 10px; background: var(--paper, #fffcf4); font-size: 14px }
summary { cursor: pointer; overflow-wrap: anywhere }
summary strong { font-weight: 650 }
summary .mail-meta, .mail-state { margin-inline-start: 10px }
.mail-content { padding-block: 6px 2px }
.mail-meta, .mail-state { color: var(--muted, #536356); font-size: 12px }
.mail-meta, .mail-feedback { margin-block: 6px }
.mail-feedback:empty { margin: 0 }
.mail-source { display: flex; align-items: center; flex-wrap: wrap; gap: 6px }
select { min-width: 0; max-width: 100%; padding: 4px 6px; font: inherit; color: inherit }
.mail-letters { list-style: none; margin: 0; padding: 0 }
.mail-letters li { padding-block: 6px; border-top: 1px solid var(--line, #d7dbcf) }
dl { display: grid; grid-template-columns: auto minmax(0, 1fr); gap: 4px 12px; margin-block: 6px; font-size: 13px }
dt { color: var(--muted, #536356) }
dd { margin: 0; overflow-wrap: anywhere; white-space: pre-wrap }
</style>
