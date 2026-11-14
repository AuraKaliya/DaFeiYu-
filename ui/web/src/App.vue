<script setup lang="ts">
import { ref } from 'vue';
import { DialogRoot, DialogTrigger, DialogPortal, DialogOverlay, DialogContent, DialogTitle, DialogDescription, DialogClose } from 'reka-ui';

const text = ref('');
const panelText = ref('');
const dialogOpen = ref(false);
const composing = ref(false);
const compositions = ref(0);
const trustedEnds = ref(0);
const verify = location.hash === '#verify';
const frameUrl = 'https://probe.dafeiyu.invalid/frames/index.html?view=1' + (verify ? '#verify' : '');

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
    <header><p class="eyebrow">DAFEIYU · U01</p><h1>大肥鱼管理中心</h1><p>离线容器、页面授权与输入交互验证</p></header>
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
      <iframe title="独立来源 Frame 验证" sandbox="allow-scripts allow-same-origin" :src="frameUrl"></iframe>
    </section>
    <footer>仅 U01 验证窗口 · 未调用模型 · 未加载业务插件</footer>
    <img src="https://network-probe.invalid/blocked.png" hidden alt="">
  </main>
</template>
