<script setup lang="ts">
import { onMounted, onUnmounted, shallowRef } from 'vue';
import { readPluginFields, type PluginPublicTarget, type PluginPublicView } from './plugin-public-settings';

const props = defineProps<PluginPublicTarget>();
const emit = defineEmits<{ stale: [] }>();
const view = shallowRef<PluginPublicView>({ status:'loading', fields:[] });
const bridge = window.chrome.webview;
const notices = {
  loading: '正在读取公开字段…',
  notDeclared: '此实例未声明公开配置字段。',
  stale: '原插件代次已过期。',
  unavailable: '暂时无法读取此实例的公开配置。'
};
let pending = '';
let timer = 0;

// 放弃本次请求并清空失效代次的公开值。
function fail(): void {
  pending = '';
  view.value = { status:'unavailable', fields:[] };
}
// 按已选择实例的原代次读取一次，超时后只有显式重试才补读。
// 步骤：固定请求号和包身份 → 请求只读投影 → 有界等候原回包。
function read(): void {
  if (pending) return;
  pending = 'field-' + crypto.randomUUID();
  view.value = { status:'loading', fields:[] };
  timer = window.setTimeout(fail, 3500);
  try {
    bridge.postMessage({ type:'settings.read', version:'1', request:pending, owner:'plugin',
      instance:props.instance, id:props.id, generation:props.generation });
  } catch {
    window.clearTimeout(timer);
    fail();
  }
}
// 只接纳原请求的公开投影，过期时让父级重新发现实际代次。
function receive(event: MessageEvent<unknown>): void {
  if (!pending) return;
  const result = readPluginFields(event.data, pending, props);
  if (!result) return;
  window.clearTimeout(timer);
  pending = '';
  view.value = result;
  if (result.status === 'stale') emit('stale');
}
// 新选择或新代次先绑定监听，再读取该代次的字段。
onMounted(() => { bridge.addEventListener('message',receive); read(); });
// 原选择消失后撤回其监听和未完成查询。
onUnmounted(() => { pending = ''; window.clearTimeout(timer); bridge.removeEventListener('message',receive); });
</script>

<template>
  <section class="public-fields" :data-fields-state="view.status" :data-fields-generation="generation" aria-labelledby="public-fields-title">
    <h4 id="public-fields-title">插件公开配置</h4>
    <dl v-if="view.status === 'ready'">
      <template v-for="field in view.fields" :key="field.id"><dt>{{ field.id }}</dt>
        <dd :data-public-field="field.id"><span>{{ field.value }}</span> <small>{{ field.source === 'user' ? '用户设置' : '包默认值' }}</small></dd>
      </template>
    </dl>
    <p v-else role="status">{{ notices[view.status] }}
      <button v-if="view.status === 'unavailable'" type="button" class="quiet-action" @click="read">重试读取</button>
    </p>
  </section>
</template>

<style scoped>
.public-fields { margin-block: 12px; font-size: 14px; overflow-wrap: anywhere }
.public-fields h4, .public-fields p, .public-fields dl { margin: 4px 0 }
.public-fields dl { display: grid; grid-template-columns: minmax(0, max-content) minmax(0, 1fr); gap: 4px 12px }
.public-fields dt, .public-fields small { color: var(--muted) }
.public-fields dd { margin: 0; white-space: pre-wrap }
</style>
