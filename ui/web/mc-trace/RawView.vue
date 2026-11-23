<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { sourceLabel } from './display.ts';
import type { Round, RecordEntry } from './rounds.ts';
import type { LineRef, StreamKind } from './trace-file.ts';
import type { LoadedTrace } from './viewer-state.ts';
import { useRecord } from './use-record.ts';

type Scope = 'round' | 'global';
interface RawItem {
  readonly ref: LineRef;
  readonly kind: string;
  readonly label?: string;
  readonly identity?: string;
}

const props = defineProps<{ source: LoadedTrace; round?: Round; scope: Scope; address?: LineRef }>();
const emit = defineEmits<{ change: [ref: LineRef | undefined, scope: Scope] }>();
const stream = ref<StreamKind>(props.address?.stream ?? 'trace');
const query = ref('');
const onlyUnlinked = ref(false);
const page = ref(0);
const full = ref(false);
const copyMessage = ref('');
const perPage = 30;
const preview = 64 * 1024;
const { value, error, pending } = useRecord(() => props.source, () => props.address);

// 保留日志事件的类型和稳定检索身份。
function hostItem(row: RecordEntry): RawItem {
  return { ref: row.ref, kind: row.kind, label: row.name, identity: row.key };
}

// 分别按两个文件的原始顺序浏览，使用行号去重同一份证据。
const rows = computed<RawItem[]>(() => {
  const index = props.source.index;
  const round = props.round;
  const local = round ? index.evidence.get(round.key) : undefined;
  const domain = props.scope === 'global'
    ? (onlyUnlinked.value ? index.unlinked : index.gameRecords)
    : [...local?.entries ?? [], ...local?.references ?? []];
  const byLine = new Map<number, RawItem>();
  for (const row of domain) {
    if (row.ref.stream !== stream.value) continue;
    byLine.set(row.ref.line, { ref: row.ref, kind: row.code, label: row.skill, identity: row.work ?? row.invocation });
  }
  if (stream.value === 'trace' && !onlyUnlinked.value) {
    const host = props.scope === 'global' ? index.records : round?.events ?? [];
    for (const entry of host) {
      if (!byLine.has(entry.ref.line)) byLine.set(entry.ref.line, hostItem(entry));
    }
  }
  return [...byLine.values()].sort((left, right) => left.ref.line - right.ref.line);
});

const filtered = computed(() => {
  const text = query.value.trim().toLowerCase();
  if (!text) return rows.value;
  if (/^\d+$/.test(text)) return rows.value.filter(item => item.ref.line === Number(text));
  return rows.value.filter(item => [item.kind, item.label, item.identity].some(value => value?.toLowerCase().includes(text)));
});
const lastPage = computed(() => Math.max(0, Math.ceil(filtered.value.length / perPage) - 1));

// 当定位来自回合卡片时切换文件并显示锚点所在的原始序列。
watch(() => props.address, address => {
  full.value = false;
  copyMessage.value = '';
  if (!address) return;
  stream.value = address.stream;
  const position = filtered.value.findIndex(item => item.ref.line === address.line);
  if (position >= 0) page.value = Math.floor(position / perPage);
}, { immediate: true });

// 查找范围变化时回到第一组原始记录。
watch([query, onlyUnlinked, () => props.scope], () => { page.value = 0; });

// 切换文件后释放旧原行的选择。
function chooseStream(next: StreamKind) {
  stream.value = next;
  page.value = 0;
  emit('change', undefined, props.scope);
}

// 复制当前所读的原始 JSONL 内容，并提示浏览器反馈。
async function copy() {
  const current = value.value;
  if (!current) return;
  try {
    await navigator.clipboard.writeText(current.raw);
    if (value.value === current) copyMessage.value = '已复制原行';
  } catch {
    if (value.value === current) copyMessage.value = '无法访问剪贴板，可在下方手动选取。';
  }
}
</script>

<template>
  <section class="detail-stack raw-view" aria-label="原始记录">
    <div class="section-heading">
      <h3>{{ scope === 'global' ? '全局日志' : '回合原始记录' }}</h3>
      <button v-if="round" type="button" class="text-button" @click="emit('change', undefined, scope === 'global' ? 'round' : 'global')">
        {{ scope === 'global' ? '查看当前回合' : '查看全局' }}
      </button>
    </div>
    <div v-if="address" class="raw-selection">
      <div class="section-heading">
        <strong class="mono">{{ sourceLabel(address) }}</strong>
        <button v-if="value" type="button" class="text-button" @click="copy">复制原行</button>
      </div>
      <p v-if="copyMessage" class="fine-print" role="status">{{ copyMessage }}</p>
      <p v-if="error" class="notice" role="alert">{{ error }}</p>
      <p v-else-if="pending" class="empty" aria-live="polite">正在读回原行…</p>
      <pre v-else-if="value" class="code-block raw-code" tabindex="0">{{ full ? value.raw : value.raw.slice(0, preview) }}</pre>
      <button v-if="value && value.raw.length > preview && !full" type="button" class="text-button" @click="full = true">展开完整原行</button>
    </div>
    <div class="raw-controls">
      <div class="segmented" aria-label="日志文件">
        <button type="button" :aria-pressed="stream === 'trace'" @click="chooseStream('trace')">宿主 Trace</button>
        <button v-if="source.game" type="button" :aria-pressed="stream === 'game'" @click="chooseStream('game')">游戏回执</button>
      </div>
      <input v-model="query" type="search" placeholder="输入行号、类型或身份" aria-label="搜索原始记录">
      <label v-if="scope === 'global'" class="checkbox"><input v-model="onlyUnlinked" type="checkbox"> 只看未关联的领域记录</label>
    </div>
    <ul class="raw-list">
      <li v-for="item in filtered.slice(page * perPage, (page + 1) * perPage)" :key="item.ref.line">
        <button type="button" :aria-current="address?.line === item.ref.line && address?.stream === item.ref.stream ? 'true' : undefined"
          @click="emit('change', item.ref, scope)">
          <span class="mono">#{{ item.ref.line }}</span><span>{{ item.label ?? item.kind }}</span>
          <code v-if="item.identity">{{ item.identity }}</code>
        </button>
      </li>
    </ul>
    <p v-if="!filtered.length" class="empty">没有符合条件的原始记录。</p>
    <div v-if="filtered.length" class="pager">
      <button class="button subtle" type="button" :disabled="page === 0" @click="page -= 1">上一批</button>
      <span>{{ page + 1 }} / {{ lastPage + 1 }} 批 · {{ filtered.length }} 条</span>
      <button class="button subtle" type="button" :disabled="page >= lastPage" @click="page += 1">下一批</button>
    </div>
  </section>
</template>
