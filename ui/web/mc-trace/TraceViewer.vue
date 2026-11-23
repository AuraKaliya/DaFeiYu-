<script setup lang="ts">
import { computed, ref, useTemplateRef, watch } from 'vue';
import { useTraceFiles } from './viewer-state.ts';
import type { Round } from './rounds.ts';
import type { ScanResult } from './trace-file.ts';

const { loaded, importing, error, open, cancel } = useTraceFiles();
const traceInput = useTemplateRef<HTMLInputElement>('trace-input');
const gameInput = useTemplateRef<HTMLInputElement>('game-input');
const selectedKey = ref('');
const rounds = computed(() => loaded.value?.index.rounds ?? []);
const selected = computed(() => rounds.value.find(round => round.key === selectedKey.value));

// 导入同批文件时取宿主索引与可选游戏回执；单独附加回执时保留宿主快照。
// 步骤：读取文件选择 → 确认输入来源 → 启动导入并清空选择框。
function pick(files: FileList | null | undefined, gameOnly = false) {
  const list = Array.from(files ?? []);
  const game = gameOnly ? list[0] : list.find(file => file.name.toLowerCase() === 'game-actions.jsonl');
  const trace = list.find(file => file !== game) ?? loaded.value?.trace;
  if (!trace || !list.length) return;
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

// 显示可复核的回合退出状态。
function roundStatus(round: Round) {
  if (!round.accepted) return '接纳待确认';
  return round.exit?.result ?? (round.exit ? '已退出' : '未见退出');
}

// 新文件选择第一回合，附加游戏回执时保持浏览位置。
watch(loaded, (next, prior) => {
  if (next?.trace === prior?.trace && selected.value) return;
  selectedKey.value = next?.index.rounds[0]?.key ?? '';
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
    <p v-if="error" class="notice" role="alert">{{ error }}</p>
    <div v-if="loaded" class="file-notices" aria-live="polite">
      <p v-if="fileStatus(loaded.index.source)" class="notice">宿主 Trace · {{ fileStatus(loaded.index.source) }}</p>
      <p v-if="loaded.index.game && fileStatus(loaded.index.game)" class="notice">游戏回执 · {{ fileStatus(loaded.index.game) }}</p>
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
      <aside class="navigator" aria-label="MC 回合">
        <div class="navigator-header"><h2>Agent 回合</h2><span class="count">{{ rounds.length }}</span></div>
        <ol class="round-list">
          <li v-for="round in rounds" :key="round.key">
            <button type="button" class="round-item" :aria-current="round.key === selectedKey ? 'true' : undefined"
              @click="selectedKey = round.key">
              <span class="round-title">{{ round.input?.trigger.label ?? '触发待确认' }}</span>
              <span class="round-meta"><code>{{ round.key }}</code> · {{ roundStatus(round) }}</span>
            </button>
          </li>
        </ol>
      </aside>
      <main v-if="selected" class="round">
        <h2>{{ selected.input?.trigger.label ?? '触发待确认' }}</h2>
        <p>{{ selected.events.length }} 条记录 · {{ selected.modelCount }} 次模型调用</p>
      </main>
      <main v-else class="round"><p class="empty">文件中没有可确认的 MC 伙伴回合。</p></main>
    </div>
  </div>
</template>
