<script setup lang="ts">
import { ref, onMounted, onBeforeUnmount } from 'vue';
import { DialogRoot, DialogTrigger, DialogPortal, DialogOverlay, DialogContent, DialogTitle, DialogDescription, DialogClose } from 'reka-ui';

const text = ref('');
const panelText = ref('');
const dialogOpen = ref(false);
const composing = ref(false);
const compositions = ref(0);
const trustedEnds = ref(0);
const verify = location.hash === '#verify' || location.hash === '#offline';
const frameUrl = ref('');
const frameVisible = ref(false);
const framePending = ref(false);
const runStatus = ref('等待读取');
let runRequest = '';

// 向主文档的宿主发送只读查询，用本次请求号筛选回执。
function readRun(): void {
  runRequest = crypto.randomUUID();
  window.chrome.webview.postMessage({ type: 'state.read', version: '1', topic: 'run', request: runRequest });
}

// 根据原业务、实际退出及读回判断生成可读状态。
// 步骤：收窄未知值 → 按审批和执行阶段分流 → 组合实际读回结果。
function runLabel(value: unknown): string | undefined {
  if (value === null) return '暂无运行';
  if (!value || typeof value !== 'object' || !('result' in value) || !('exit' in value) ||
      !('waiting_confirmations' in value) || typeof value.waiting_confirmations !== 'number') return;
  if (value.waiting_confirmations > 0) return '等待审批';
  switch (value.result) {
    case 'pending': return '运行中';
    case 'cancelled': return value.exit === 'pending' ? '已取消，正在收尾' : '已取消';
    case 'failed': return value.exit === 'pending' ? '运行失败，正在收尾' : '运行失败';
    case 'succeeded':
      if (value.exit === 'pending') return '任务完成，正在收尾';
      return 'verification' in value && value.verification === 'satisfied' ? '已完成，读回已核验' : '已完成';
  }
}

// 只有原生宿主确认旧 Frame 已销毁后才重新申请，不由页面猜测代次。
function openFrame(): void { framePending.value = true; window.chrome.webview.postMessage({ type: 'frame.open' }); }
// 移除 DOM 后仍等待真实销毁回执，避免过早重新占用通道。
function closeFrame(): void { framePending.value = true; frameVisible.value = false; }
// 校验宿主回执再更新视图，非法数据不能注入任意 iframe 地址。
// 步骤：收窄未知消息 → 应用本次运行查询或原生 Frame 回执。
function hostMessage(event: MessageEvent<unknown>): void {
  const data = event.data;
  if (!data || typeof data !== 'object' || !('event' in data)) return;
  if (data.event === 'state.snapshot' && 'version' in data && data.version === '1' && 'topic' in data && data.topic === 'run' &&
      'request' in data && data.request === runRequest && 'available' in data && typeof data.available === 'boolean' && 'run' in data) {
    const label = data.available ? runLabel(data.run) : '业务宿主未连接';
    if (label !== undefined) runStatus.value = label;
  } else if (data.event === 'frame.opened' && 'url' in data && typeof data.url === 'string' && /^https:\/\/probe\.dafeiyu\.invalid\/frames\/index\.html\?view=[1-9][0-9]*$/.test(data.url)) {
    frameUrl.value = data.url + (verify ? location.hash : ''); frameVisible.value = true; framePending.value = false;
  } else if (data.event === 'frame.closed') framePending.value = false;
  else if (data.event === 'frame.busy') framePending.value = true;
}
// 先登记回执再申请 Frame，避免漏掉快速返回的宿主身份。
// 步骤：登记当前文档监听 → 请求运行快照 → 按探针范围申请 Frame。
onMounted(() => {
  window.chrome.webview.addEventListener('message', hostMessage);
  readRun();
  if (location.hash !== '#run-status') openFrame();
});
// 只撤销当前页面监听，不代替原生 Frame 的实际退出。
onBeforeUnmount(() => window.chrome.webview.removeEventListener('message', hostMessage));

// 组合状态按事件结束恢复，浏览器可信标记单独记录，不把协议模拟等同于系统输入法。
// 步骤：按事件种类更新组合状态 → 记录结束次数与可信标记。
function composition(event: CompositionEvent): void {
  composing.value = event.type === 'compositionstart';
  if (event.type === 'compositionend') { compositions.value++; if (event.isTrusted) trustedEnds.value++; }
}
// 输入法组合期间保留 Escape 给候选窗口，不把取消候选误当作关闭面板。
function escape(event: KeyboardEvent): void {
  if (event.isComposing || composing.value) event.preventDefault();
}
</script>

<template>
  <main>
    <header><p class="eyebrow">DAFEIYU</p><h1>大肥鱼管理中心</h1></header>
    <section class="surface">
      <h2>运行状态</h2>
      <p id="run-status" role="status">{{ runStatus }}</p>
      <button id="refresh-run" @click="readRun">刷新状态</button>
    </section>
    <section class="surface">
      <h2>输入与焦点</h2>
      <div class="fields">
        <label for="main-text">主界面中文输入<input id="main-text" v-model="text" placeholder="使用中文输入法输入文字" @compositionstart="composition" @compositionend="composition"></label>
        <label for="next-text">下一个焦点<input id="next-text" placeholder="Tab 应当到达这里"></label>
      </div>
      <p aria-live="polite">组合结束次数：<strong id="composition-count">{{ compositions }}</strong> · 可信标记：<span id="trusted-composition-count">{{ trustedEnds }}</span> · {{ composing ? '正在选字' : '未组合' }}</p>
      <p class="echo">当前文字：<span id="main-echo">{{ text || '尚未输入' }}</span></p>
      <DialogRoot v-model:open="dialogOpen">
        <DialogTrigger id="open-panel" class="primary">打开验证面板</DialogTrigger>
        <DialogPortal><DialogOverlay class="overlay" />
          <DialogContent class="dialog" @escape-key-down="escape">
            <DialogTitle class="dialog-title">输入与焦点验证</DialogTitle>
            <DialogDescription>Tab 应保持在面板内。输入法选字时 Escape 取消候选，结束组合后 Escape 关闭面板。</DialogDescription>
            <label for="panel-text">面板中文输入<input id="panel-text" v-model="panelText" @compositionstart="composition" @compositionend="composition"></label>
            <label for="panel-next">面板第二个焦点<input id="panel-next" placeholder="检查 Tab 与 Shift+Tab"></label>
            <DialogClose id="close-panel" class="primary">关闭面板</DialogClose>
          </DialogContent>
        </DialogPortal>
      </DialogRoot>
    </section>
    <section class="surface">
      <h2>独立来源页面</h2><p>此区域使用不同来源的 iframe；页面消息须由原生宿主授权。</p>
      <div class="frame-actions"><button id="remove-frame" :disabled="!frameVisible || framePending" @click="closeFrame">移除验证页面</button><button id="reopen-frame" :disabled="frameVisible || framePending" @click="openFrame">重新打开页面</button><span>{{ framePending ? '等待原生确认' : '页面可操作' }}</span></div>
      <iframe v-if="frameVisible" title="独立来源 Frame 验证" sandbox="allow-scripts allow-same-origin" :src="frameUrl"></iframe>
    </section>
    <img src="https://network-probe.invalid/blocked.png" hidden alt="">
  </main>
</template>
