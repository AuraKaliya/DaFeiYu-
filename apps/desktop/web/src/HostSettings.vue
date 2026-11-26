<script setup lang="ts">
import { onMounted, onUnmounted, watch } from 'vue';
import { useHostSettings } from './host-settings';

const props = defineProps<{ active: boolean; instance: string }>();
const bridge = window.chrome.webview;
const settings = useHostSettings(fields => bridge.postMessage(fields));
const { view } = settings;
let timer = 0;
// 只接收原管理主文档中匹配的主体设置回复。
function receive(event: MessageEvent<unknown>): void { settings.receive(event.data); }
// 根据已握手后台和当前设置可见性发起读取。
function update(): void { settings.context(props.instance, props.active); }
watch(() => [props.instance, props.active], update);
// 在可见设置中开始独立观察。
onMounted(() => { bridge.addEventListener('message', receive); update(); timer = window.setInterval(settings.tick, 500); });
// 文档卸载时撤回监听和未完成的读取。
onUnmounted(() => { window.clearInterval(timer); bridge.removeEventListener('message', receive); settings.context('', false); });
</script>

<template>
  <section class="host-settings" :data-settings-state="view.status" :data-settings-instance="view.reading?.instance"
    :data-settings-revision="view.reading?.revision" aria-labelledby="host-settings-heading">
    <div class="heading"><h3 id="host-settings-heading" class="group-title">本体设置</h3>
      <button id="refresh-host-settings" type="button" class="quiet-action" :disabled="!instance" @click="settings.refresh">刷新设置</button>
    </div>
    <p v-if="view.status !== 'ready'" class="source-note" role="status">
      {{ view.status === 'loading' ? '正在读取本体设置…' : view.status === 'stale' ? '本体设置已过期。' : '暂时无法读取本体设置。' }}
      {{ view.reading ? '以下是上次核实的值。' : '' }}
    </p>
    <template v-if="view.reading">
      <dl class="values">
        <dt>模型供应商</dt><dd id="host-provider">{{ view.reading.appliedProvider }}</dd>
        <dt>当前生效超时</dt><dd id="applied-timeout">{{ view.reading.appliedTimeout }} 毫秒</dd>
        <dt>已保存超时</dt><dd id="saved-timeout">{{ view.reading.timeout }} 毫秒</dd>
        <dt>配置来源</dt><dd>{{ view.reading.source === 'user' ? '用户设置' : '启动配置' }}</dd>
      </dl>
      <p class="source-note">已保存修订 {{ view.reading.revision }} · 当前生效修订 {{ view.reading.appliedRevision }}
        <span v-if="view.reading.pendingRestart"> · 下次启动本体时采用已保存超时</span></p>
    </template>
  </section>
</template>

<style scoped>
.host-settings { max-inline-size: 48rem; margin-block-end: 24px }
.heading { display: flex; align-items: center; flex-wrap: wrap; justify-content: space-between; gap: 8px }
.heading h3 { margin: 0 }
.values { display: grid; grid-template-columns: max-content minmax(0, 1fr); gap: 4px 12px; margin: 8px 0; font-size: 14px }
.values dt { color: var(--muted) }
.values dd { margin: 0; overflow-wrap: anywhere }
.source-note { margin: 6px 0 }
</style>
