<script setup lang="ts">
import { computed, onMounted, onUnmounted, watch } from 'vue';
import { Label } from 'reka-ui';
import { usePluginDirectory } from './plugin-directory';

const props = defineProps<{ active: boolean; connected: boolean }>();
const bridge = window.chrome.webview;
const directory = usePluginDirectory(fields => bridge.postMessage(fields));
const { view } = directory;
const selected = computed(() => view.value.reading?.packages.find(item => item.id === view.value.selected));
const labels: Readonly<Record<string, string>> = { installed: '已安装', active: '已加载', stopping: '正在卸载', failed: '实例异常' };
let timer = 0;

// 用当前读数解释列表范围，只有已核实的空目录显示空状态。
const feedback = computed(() => {
  const { status, reading } = view.value;
  if (status === 'ready' && reading) return reading.packages.length ? `共 ${reading.packages.length} 个插件包。` : '当前插件目录为空。';
  if (status === 'stale') return reading ? '目录已过期，显示上次核实的列表。' : '目录已过期，请重新读取。';
  if (status === 'unavailable') return reading ? '暂时无法读取插件目录，显示上次核实的列表。' : '暂时无法读取插件目录。';
  return reading ? '正在重新读取插件目录，显示上次核实的列表。' : '正在读取插件目录…';
});

// 原生回包由当前文档的观察器核对身份。
function receive(event: MessageEvent<unknown>): void { directory.receive(event.data); }
// 随后台连接和设置可见性调整查询。
function update(): void { directory.context(props.connected, props.active); }
watch(() => [props.active, props.connected], update);
// 先接收原生消息，再开始可见页面的有界轮询。
onMounted(() => {
  bridge.addEventListener('message', receive);
  update();
  timer = window.setInterval(directory.tick, 500);
});
// 归还当前文档的监听和定时器，晚到回复失效。
onUnmounted(() => {
  window.clearInterval(timer);
  bridge.removeEventListener('message', receive);
  directory.context(false, false);
});
</script>

<template>
  <section class="plugin-settings" :data-directory-status="view.status" aria-labelledby="settings-plugins-title">
    <div class="plugin-heading">
      <h3 id="settings-plugins-title" class="group-title">插件管理</h3>
      <button id="refresh-plugins" type="button" class="quiet-action" :disabled="!connected" @click="directory.refresh">刷新目录</button>
    </div>
    <p id="plugin-directory-status" class="source-note" role="status">{{ feedback }} {{ view.notice }}</p>
    <fieldset v-if="view.reading?.packages.length" class="packages" :disabled="view.status !== 'ready'">
      <legend>选择插件</legend>
      <Label v-for="item in view.reading.packages" :key="item.id" :data-package="item.id" class="package" :data-selected="view.selected === item.id">
        <input type="radio" name="managed-package" :value="item.id" :checked="view.selected === item.id" @change="directory.select(item.id)">
        <span class="package-name">{{ item.name || item.id }}</span>
        <span class="package-version">{{ item.version }}</span>
        <span class="package-state">{{ labels[item.state] ?? item.state }}</span>
      </Label>
    </fieldset>
    <dl v-if="selected" class="package-details" :data-selected-package="selected.id">
      <dt>插件 ID</dt><dd>{{ selected.id }}</dd>
      <template v-if="selected.generation"><dt>加载代次</dt><dd>{{ selected.generation }}</dd></template>
    </dl>
    <p v-if="view.reading" class="plugin-revision">目录修订 {{ view.reading.revision }}</p>
  </section>
</template>

<style scoped>
.plugin-settings { max-inline-size: 48rem }
.plugin-heading { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 8px }
.plugin-heading .group-title { margin: 0 }
.plugin-settings .source-note { margin: 6px 0 10px }
.packages { display: grid; min-inline-size: 0; margin: 0; padding: 0; border: 0; border-block-start: 1px solid var(--line) }
.packages legend { position: absolute; width: 1px; height: 1px; padding: 0; overflow: hidden; clip-path: inset(50%) }
.package { display: grid; grid-template-columns: auto minmax(0, 1fr) minmax(0, auto) minmax(0, auto); align-items: baseline;
  gap: 10px; width: 100%; padding: 9px 10px; text-align: start; background: transparent; color: var(--ink);
  border: 0; border-block-end: 1px solid var(--line); font: inherit; cursor: pointer; overflow-wrap: anywhere }
.package[data-selected="true"] { background: #e2ede6 }
.package input { align-self: center; margin: 0; accent-color: var(--accent) }
.packages:disabled .package { cursor: default }
.package-name { font-weight: 600 }
.package-state, .package-version, .plugin-revision { color: var(--muted); font-size: 13px }
.package-details { display: grid; grid-template-columns: max-content minmax(0, 1fr); gap: 4px 12px; font-size: 14px; margin: 12px 0 6px }
.package-details dt { color: var(--muted) }
.package-details dd { margin: 0; overflow-wrap: anywhere }
.plugin-revision { margin: 6px 0; overflow-wrap: anywhere }
</style>
