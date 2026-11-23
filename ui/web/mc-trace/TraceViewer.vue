<script setup lang="ts">
import { computed, nextTick, ref, useTemplateRef, watch } from 'vue';
import RoundList from './RoundList.vue';
import RunTimeline from './RunTimeline.vue';
import PromptView from './PromptView.vue';
import OutcomeView from './OutcomeView.vue';
import McEvidenceView from './McEvidenceView.vue';
import RawView from './RawView.vue';
import { modelStages } from './model-stages.ts';
import { useTraceFiles } from './viewer-state.ts';
import type { RoundLink } from './mc-links.ts';
import type { LineRef, ScanResult } from './trace-file.ts';

const { loaded, importing, error, open, cancel } = useTraceFiles();
const traceInput = useTemplateRef<HTMLInputElement>('trace-input');
const gameInput = useTemplateRef<HTMLInputElement>('game-input');
const inspector = useTemplateRef<HTMLElement>('inspector');
const selectedKey = ref('');
const selectionError = ref('');
const traceRevision = ref(0);
const stageId = ref('');
type Panel = 'prompt' | 'outcome' | 'mc' | 'raw';
const panel = ref<Panel>('prompt');
const rawRef = ref<LineRef>();
const rawScope = ref<'round' | 'global'>('round');
const rounds = computed(() => loaded.value?.index.rounds ?? []);
const selected = computed(() => rounds.value.find(round => round.key === selectedKey.value));
const stages = computed(() => selected.value ? modelStages(selected.value) : []);
const stage = computed(() => stages.value.find(entry => entry.key === stageId.value));
const evidence = computed(() => loaded.value?.index.evidence.get(selectedKey.value));
const unlinkedGame = computed(() => loaded.value?.index.unlinked.filter(row => row.ref.stream === 'game').length ?? 0);
const earlierPrepared = computed(() => {
  const active = stages.value.findIndex(entry => entry.key === stageId.value);
  if (active > 0) return stages.value.slice(0, active).reverse().find(entry => entry.prepared)?.prepared;
});

// 导入同批文件时取宿主索引与可选游戏回执；单独附加回执时保留宿主快照。
// 步骤：读取文件选择 → 确认输入来源 → 启动导入并清空选择框。
function pick(files: FileList | null | undefined, gameOnly = false) {
  const list = Array.from(files ?? []);
  if (!list.length) return;
  selectionError.value = '';
  const game = gameOnly ? list[0] : list.find(file => file.name.toLowerCase() === 'game-actions.jsonl');
  if (list.filter(file => file !== game).length > 1) {
    selectionError.value = '一次请选择一份宿主 Trace，可同时附带同局的 game-actions.jsonl。';
    return;
  }
  const trace = list.find(file => file !== game) ?? loaded.value?.trace;
  if (!trace) {
    selectionError.value = '请先打开宿主 events.jsonl，再附加本局游戏回执。';
    return;
  }
  open({ trace, game });
}

// 清空浏览器保存的选项，使复选同一文件时也触发读取新快照。
function change(event: Event, gameOnly = false) {
  const input = event.currentTarget as HTMLInputElement;
  pick(input.files, gameOnly);
  input.value = '';
}

// 按日志状态生成文件检查结果。
function fileStatus(state: ScanResult) {
  if (state.issue) return state.issue;
  return state.state === 'open' ? '未见 Trace 结束标记' : '';
}

// 选择回合并清除来自上一次浏览的原始行定位。
function select(key: string) {
  selectedKey.value = key;
  stageId.value = stages.value.find(entry => entry.prepared)?.key ?? stages.value[0]?.key ?? '';
  panel.value = stageId.value ? 'prompt' : 'mc';
  rawRef.value = undefined;
  rawScope.value = 'round';
}

// 将新选中的详情带入视口，窄屏时定位到独立详情区域。
// 步骤：等待内容挂载 → 展开选中的工具 → 定位详情或回到面板顶部。
async function revealInspector(tool?: string) {
  await nextTick();
  const target = inspector.value;
  if (!target) return;
  const selected = tool ? target.querySelector<HTMLDetailsElement>(`[data-tool="${CSS.escape(tool)}"]`) : undefined;
  if (selected) {
    selected.open = true;
    selected.scrollIntoView({ block: 'nearest' });
  } else if (matchMedia('(max-width: 1050px)').matches) {
    target.scrollIntoView({ block: 'start' });
  } else {
    target.scrollTop = 0;
  }
}

// 打开详情后将阅读位置复位到可见处。
function show(next: Panel) {
  panel.value = next;
  void revealInspector();
}

// 选择送模尝试，可直接查看具体工具的裁定和结果。
function selectStage(key: string, tool?: string) {
  stageId.value = key;
  panel.value = tool === undefined ? 'prompt' : 'outcome';
  void revealInspector(tool);
}

// 在右侧固定一条已归属或未归属的原始记录。
function inspect(ref?: LineRef, global = false) {
  rawRef.value = ref;
  rawScope.value = global ? 'global' : 'round';
  show('raw');
}

// 跳到原工作或后继回合，带定位的链接打开对应原行。
function follow(link: RoundLink) {
  select(link.round);
  if (link.ref) inspect(link.ref);
  else show('mc');
}

// 新文件选择第一回合，附加游戏回执时保持浏览位置。
watch(loaded, (next, prior) => {
  if (next?.trace === prior?.trace && selected.value) {
    if (next?.game !== prior?.game && rawRef.value?.stream === 'game') rawRef.value = undefined;
    return;
  }
  traceRevision.value += 1;
  select(next?.index.rounds[0]?.key ?? '');
  if (!selected.value) {
    panel.value = 'raw';
    rawScope.value = 'global';
  }
});
</script>

<template>
  <div class="app-shell" @dragover.prevent @drop.prevent="pick($event.dataTransfer?.files)">
    <header class="topbar">
      <div class="brand">
        <span class="brand-mark" aria-hidden="true"><i></i><i></i><i></i><i></i></span>
        <div><h1>MC Trace 查看器</h1><small>大肥鱼 · Minecraft 伙伴</small></div>
      </div>
      <div class="top-files" v-if="loaded">
        <span :title="loaded.trace.name">{{ loaded.trace.name }} · {{ loaded.index.source.lines }} 条</span>
        <span v-if="loaded.game" :title="loaded.game.name">{{ loaded.game.name }} · {{ loaded.index.game?.lines }} 条</span>
      </div>
      <div class="file-actions">
        <button v-if="loaded" class="button on-dark" type="button" @click="gameInput?.click()">附加游戏回执</button>
        <button class="button primary" type="button" @click="traceInput?.click()">打开 Trace</button>
      </div>
      <input id="trace-input" ref="trace-input" type="file" accept=".jsonl,.ndjson,.log" multiple hidden @change="change($event)">
      <input id="game-input" ref="game-input" type="file" accept=".jsonl,.ndjson,.log" hidden @change="change($event, true)">
    </header>

    <section v-if="importing" class="import-notice" aria-live="polite">
      <span>读取 {{ importing.file }}</span>
      <progress :value="importing.fraction" max="1" aria-label="文件读取进度"></progress>
      <button class="button subtle" type="button" @click="cancel">取消</button>
    </section>
    <p v-if="selectionError || error" class="notice" role="alert">{{ selectionError || error }}</p>
    <div v-if="loaded" class="file-notices" aria-live="polite">
      <p v-if="fileStatus(loaded.index.source)" class="notice">宿主 Trace · {{ fileStatus(loaded.index.source) }}</p>
      <p v-if="loaded.index.game && fileStatus(loaded.index.game)" class="notice">游戏回执 · {{ fileStatus(loaded.index.game) }}</p>
      <p v-if="unlinkedGame" class="notice">
        {{ unlinkedGame }} 条游戏回执无法关联到回合。
        <button class="text-button" type="button" @click="inspect(undefined, true)">查看全局日志</button>
      </p>
    </div>

    <main v-if="!loaded" class="welcome">
      <div class="welcome-card">
        <h2>打开一局 Minecraft Trace</h2>
        <p>选择 <code>events.jsonl</code>，按回合检查实际送模、程序裁定和游戏效果。</p>
        <p>可同时选择同局的 <code>game-actions.jsonl</code>，也可以把文件拖到这里。</p>
        <button class="button accent" type="button" @click="traceInput?.click()">选择本地文件</button>
      </div>
    </main>
    <div v-else class="workspace">
      <RoundList :key="traceRevision" :rounds="rounds" :selected="selectedKey"
        @select="select" @raw="inspect(undefined, true)" />
      <RunTimeline v-if="selected" :key="selectedKey" :round="selected" :stages="stages" :active="stageId"
        :evidence-count="(evidence?.entries.length ?? 0) + (evidence?.references.length ?? 0)"
        @stage="selectStage" @record="inspect" @mc="show('mc')" />
      <main v-else class="round"><p class="empty">文件中没有可确认的 MC 伙伴回合。</p></main>
      <aside ref="inspector" class="inspector" aria-label="记录详情">
        <header v-if="selected" class="inspector-header" aria-label="详情切换">
          <button type="button" :aria-pressed="panel === 'prompt'" @click="show('prompt')">实际送模</button>
          <button type="button" :aria-pressed="panel === 'outcome'" @click="show('outcome')">响应与裁定</button>
          <button type="button" :aria-pressed="panel === 'mc'" @click="show('mc')">MC 实效</button>
          <button type="button" :aria-pressed="panel === 'raw'" @click="show('raw')">原始记录</button>
        </header>
        <PromptView v-if="panel === 'prompt' && stage?.prepared" :key="selectedKey + '/' + stage.key"
          :source="loaded" :prepared="stage.prepared" :previous="earlierPrepared" @inspect="inspect" />
        <p v-else-if="panel === 'prompt'" class="empty">所选阶段未见装配记录。</p>
        <OutcomeView v-else-if="panel === 'outcome' && selected" :key="selectedKey + '/' + stageId"
          :source="loaded" :round="selected" :stage="stage" @inspect="inspect" />
        <McEvidenceView v-else-if="panel === 'mc'" :key="selectedKey" :source="loaded" :evidence="evidence"
          @inspect="inspect" @follow="follow" />
        <RawView v-else :source="loaded" :round="selected" :scope="rawScope" :address="rawRef"
          @change="(ref, scope) => inspect(ref, scope === 'global')" />
      </aside>
    </div>
  </div>
</template>
