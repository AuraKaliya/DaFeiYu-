<script setup lang="ts">
import { computed, onMounted, onUnmounted, watch } from 'vue';
import { useHostSettings } from './host-settings';
import { useHostSettingEdit } from './host-settings-edit';
import { windowReceiptStorage } from './receipt-storage';

const props = defineProps<{ active: boolean; instance: string }>();
const bridge = window.chrome.webview;
const settings = useHostSettings(fields => bridge.postMessage(fields));
const edit = useHostSettingEdit(settings, fields => bridge.postMessage(fields), windowReceiptStorage());
const { view } = settings;
const { draft, receipt, lookupNotice } = edit;
const staleDraft = computed(() => !!draft.value && draft.value.instance === view.value.reading?.instance && draft.value.revision !== view.value.reading.revision);
const notices: Readonly<Record<string,string>> = {
  sending:'正在提交保存。', uncertain:'保存结果待确认，正在按原提交号查询。', unknown:'当前配置未查到这笔提交的保存记录。',
  conflict:'其他窗口已修改配置，本次保存未写入。', submissionConflict:'提交号与已有记录冲突，本次保存未写入。',
  readOnly:'这个设置只读。', invalid:'后台拒绝了这个超时值。', failed:'写入配置失败。', storageChanged:'磁盘上的配置已变更，后台未写入。',
  stale:'后台已更换，本次保存未写入。', unavailable:'后台未接纳本次保存。',
};
let timer = 0;
// 两个读口分别处理当前配置及原保存实效，仅在活动窗口继续查回。
// 步骤：匹配配置快照 → 核对原提交回执 → 可见时继续补读。
function receive(event: MessageEvent<unknown>): void {
  settings.receive(event.data); edit.receive(event.data);
  if (props.active && props.instance) edit.tick();
}
// 主体设置可见时推进读取及原提交查询。
function tick(): void { settings.tick(); if (props.active && props.instance) edit.tick(); }
// 保留原输入文本，非法和不完整的输入不进入保存请求。
function change(event: Event): void { edit.input((event.currentTarget as HTMLInputElement).value); }
// 根据已握手后台和当前设置可见性发起读取。
function update(): void { settings.context(props.instance, props.active); }
watch(() => [props.instance, props.active], update);
// 在可见设置中开始独立观察。
// 步骤：挂接监听 → 核对当前后台 → 安排有界轮询。
onMounted(() => { bridge.addEventListener('message', receive); update(); timer = window.setInterval(tick, 500); });
// 文档卸载时撤回监听和未完成的读取。
// 步骤：停止轮询 → 释放监听 → 撤销原文档的查询。
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
      <form v-if="view.reading.canSave" class="timeout-edit" aria-labelledby="host-settings-heading" @submit.prevent="edit.save">
        <label for="host-timeout-input">请求超时（毫秒）</label>
        <div class="input-row">
          <input id="host-timeout-input" type="number" inputmode="numeric" required step="1" :min="view.reading.min" :max="view.reading.max"
            :value="draft?.instance === view.reading.instance ? draft.text : view.reading.timeout" :disabled="view.status !== 'ready' || edit.busy()"
            aria-describedby="host-timeout-range" :aria-invalid="!!draft && edit.value() === undefined" @input="change">
          <button id="save-host-timeout" type="submit" class="primary-action" :disabled="!edit.canSave()">保存超时</button>
        </div>
        <small id="host-timeout-range">允许 {{ view.reading.min }} 至 {{ view.reading.max }} 毫秒。</small>
        <div v-if="staleDraft" class="draft-conflict" role="status">
          <p>配置修订已变更，你的输入仍保留。当前已保存 {{ view.reading.timeout }} 毫秒。</p>
          <button id="rebase-host-timeout" type="button" class="quiet-action" :disabled="view.status !== 'ready' || edit.busy()" @click="edit.rebase">保留输入并使用新修订</button>
          <button type="button" class="quiet-action" :disabled="edit.busy()" @click="edit.reset">采用当前值</button>
        </div>
      </form>
      <div v-if="receipt" class="settings-receipt" :data-save-state="receipt.state">
        <p role="status">{{ receipt.state === 'saved' ? `已保存 ${receipt.timeout} 毫秒，保存修订 ${receipt.savedRevision}。` : notices[receipt.state] }} {{ lookupNotice }}</p>
        <details><summary>保存回执</summary><p>提交号 {{ receipt.submission }}</p>
          <button id="check-host-save" type="button" class="quiet-action" :disabled="view.status !== 'ready'" @click="edit.check">查询原提交</button>
        </details>
      </div>
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
.timeout-edit { display: grid; gap: 4px; justify-items: start; margin-block: 12px }
.input-row { display: flex; align-items: center; flex-wrap: wrap; gap: 8px }
.input-row input { inline-size: 9rem; max-inline-size: 100%; padding: 6px 8px; border: 1px solid var(--line); border-radius: 6px; font: inherit }
.timeout-edit small, .settings-receipt summary { color: var(--muted) }
.settings-receipt, .draft-conflict { font-size: 14px; overflow-wrap: anywhere }
.settings-receipt p, .draft-conflict p { margin: 5px 0 }
.settings-receipt summary { cursor: pointer }
</style>
