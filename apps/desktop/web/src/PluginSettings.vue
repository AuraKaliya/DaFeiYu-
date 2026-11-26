<script setup lang="ts">
import { computed, onMounted, onUnmounted, watch } from 'vue';
import { Label } from 'reka-ui';
import { usePluginDirectory } from './plugin-directory';
import { usePluginActions } from './plugin-actions';
import { windowReceiptStorage } from './receipt-storage';

const props = defineProps<{ active: boolean; connected: boolean }>();
const bridge = window.chrome.webview;
const directory = usePluginDirectory(fields => bridge.postMessage(fields));
const actions = usePluginActions(directory, fields => bridge.postMessage(fields), windowReceiptStorage());
const { view } = directory;
const { work, readFailed, lookupNotice } = actions;
const selected = computed(() => view.value.reading?.packages.find(item => item.id === view.value.selected));
const labels: Readonly<Record<string, string>> = { installed: '已安装', active: '已加载', stopping: '正在卸载', failed: '实例异常' };
let timer = 0;

// 用当前读数解释列表范围，只有已核实的空目录显示空状态。
const feedback = computed(() => {
  const { status, reading } = view.value;
  if (status === 'ready' && reading) {
    if (reading.unavailableCount) return `已读到 ${reading.packages.length} 个插件包，另有 ${reading.unavailableCount} 个目录项无法读取。`;
    return reading.packages.length ? `共 ${reading.packages.length} 个插件包。` : '当前插件目录为空。';
  }
  if (status === 'stale') return reading ? '目录已过期，显示上次核实的列表。' : '目录已过期，请重新读取。';
  if (status === 'unavailable') return reading ? '暂时无法读取插件目录，显示上次核实的列表。' : '暂时无法读取插件目录。';
  return reading ? '正在重新读取插件目录，显示上次核实的列表。' : '正在读取插件目录…';
});

// 操作文案固定在原意图，区分接纳、实例保留和退出实效。
const result = computed(() => {
  const item = work.value;
  if (!item) return '';
  if (view.value.reading?.instance !== item.instance) return '后台实例已更换，此回执属于原后台。';
  if (item.state === 'sending') return '正在提交，等待后台接纳。';
  if (item.state === 'queued') return '后台已接纳，正在排队。';
  if (item.state === 'running') return '后台正在执行。';
  if (item.state === 'pending') return '已请求停止，正在等待原实例退出。';
  if (item.state === 'uncertain') return '尚未收到接纳回执，操作结果待确认。';
  if (item.state === 'succeeded') return item.effect === 'loaded' ? `已加载，运行代次 ${item.generation}。` : `原实例（代次 ${item.targetGeneration}）已卸载。`;
  if (item.effect === 'retained') {
    if (item.state === 'inUse') return '仍有插件依赖这个实例，原实例已保留。';
    return item.state === 'busy' ? '插件正忙，原实例已保留。' : '操作失败，原实例已保留。';
  }
  if (item.state === 'stale') return item.effect === 'unknown' ? '原后台已更换，原操作实效待确认。' : '后台、目录修订或目标代次已变化，本次操作未执行。';
  if (item.state === 'outOfOrder' || item.state === 'conflict') return '后台拒绝了该命令号，本次操作未执行。';
  if (item.state === 'expired' || item.state === 'unknown') return '后台无法确认原操作记录，实效待确认。';
  if (item.effect === 'notLoaded') return '加载失败，插件未加载。';
  return item.effect === 'notApplied' ? '后台未执行本次操作。' : '操作实效待确认。';
});

// 原生回包分别核对目录和命令的原编号，后续查询只在可见设置发生。
// 步骤：接纳目录或回执 → 根据当前后台安排原操作补读。
function receive(event: MessageEvent<unknown>): void {
  directory.receive(event.data); actions.receive(event.data);
  if (props.active && props.connected) actions.tick();
}
// 在活动页面按各自期限推进目录和待结算回执。
function tick(): void {
  directory.tick();
  if (props.active && props.connected) actions.tick();
}
// 随后台连接和设置可见性调整查询。
function update(): void { directory.context(props.connected, props.active); }
watch(() => [props.active, props.connected], update);
// 先接收原生消息，再开始可见页面的有界轮询。
onMounted(() => {
  bridge.addEventListener('message', receive);
  update();
  timer = window.setInterval(tick, 500);
});
// 归还当前文档的监听和定时器，晚到回复失效。
onUnmounted(() => {
  window.clearInterval(timer);
  bridge.removeEventListener('message', receive);
  directory.context(false, false);
});
</script>

<template>
  <section class="plugin-settings" :data-directory-status="view.status" :data-directory-instance="view.reading?.instance"
    :data-directory-revision="view.reading?.revision" aria-labelledby="settings-plugins-title">
    <div class="plugin-heading">
      <h3 id="settings-plugins-title" class="group-title">插件管理</h3>
      <button id="refresh-plugins" type="button" class="quiet-action" :disabled="!connected" @click="directory.refresh">刷新目录</button>
    </div>
    <p id="plugin-directory-status" class="source-note" role="status">{{ feedback }} {{ view.notice }}</p>
    <fieldset v-if="view.reading?.packages.length" class="packages" :disabled="view.status !== 'ready'">
      <legend>选择插件</legend>
      <Label v-for="(item, index) in view.reading.packages" :key="item.id" :for="'managed-plugin-' + index" :data-package="item.id" class="package" :data-selected="view.selected === item.id">
        <input :id="'managed-plugin-' + index" type="radio" name="managed-package" :value="item.id" :checked="view.selected === item.id" @change="directory.select(item.id)">
        <span class="package-name">{{ item.name || item.id }}</span>
        <span class="package-version">{{ item.version }}</span>
        <span class="package-state">{{ labels[item.state] }}</span>
      </Label>
    </fieldset>
    <dl v-if="selected" class="package-details" :data-selected-package="selected.id" :data-selected-generation="selected.generation">
      <dt>插件 ID</dt><dd>{{ selected.id }}</dd>
      <dt>安装包</dt><dd>{{ selected.installed ? selected.version : '目录中已移除' }}</dd>
      <template v-if="selected.loaded">
        <dt>运行实例</dt><dd>{{ selected.loadedName }} {{ selected.loadedVersion }}</dd>
        <dt>加载代次</dt><dd>{{ selected.generation }}</dd>
        <template v-if="selected.errorCode"><dt>实例错误码</dt><dd>{{ selected.errorCode }}</dd></template>
      </template>
    </dl>
    <div v-if="selected" class="package-actions">
      <button v-if="!selected.loaded" id="plugin-load" type="button" class="primary-action" :disabled="!actions.canStart('load')" @click="actions.start('load')">加载插件</button>
      <button v-else id="plugin-unload" type="button" class="quiet-action" :disabled="!actions.canStart('unload')" @click="actions.start('unload')">卸载插件</button>
    </div>
    <div v-if="work" class="plugin-operation" :data-work-state="work.state" :data-work-effect="work.effect" :data-work-target="work.id">
      <p role="status"><strong>{{ work.action === 'load' ? '加载' : '卸载' }} {{ work.id }}</strong> · {{ result }}
        <span v-if="readFailed && work.operation && work.instance === view.reading?.instance">原回执暂时无法读取。</span>
        <span v-else-if="lookupNotice">{{ lookupNotice }}</span></p>
      <details v-if="work.operation">
        <summary>操作回执</summary>
        <dl class="package-details">
          <dt>操作号</dt><dd>{{ work.operation }}</dd><dt>提交时目录修订</dt><dd>{{ work.revision }}</dd>
          <template v-if="work.targetGeneration !== '0'"><dt>目标代次</dt><dd>{{ work.targetGeneration }}</dd></template>
          <template v-if="work.nativeCode !== undefined"><dt>插件返回码</dt><dd>{{ work.nativeCode }}</dd></template>
          <template v-if="work.nativeResult"><dt>卸载返回值</dt><dd>{{ work.nativeResult }}</dd></template>
        </dl>
        <button id="check-plugin-receipt" type="button" class="quiet-action"
          :disabled="view.status !== 'ready' || work.instance !== view.reading?.instance" @click="actions.check">重新查询回执</button>
      </details>
    </div>
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
.package-actions { margin-block: 8px }
.plugin-operation { margin-block: 10px; overflow-wrap: anywhere; font-size: 14px }
.plugin-operation p { margin: 0 0 5px }
.plugin-operation summary { cursor: pointer; color: var(--muted) }
</style>
