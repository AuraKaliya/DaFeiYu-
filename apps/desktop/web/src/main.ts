import { createApp } from 'vue';

// 显式房间预览只装配本地样本；其余入口沿用原协议页面。
// 步骤：按受信文档片段选入口 → 加载对应资源 → 挂载独立视图。
async function mount(): Promise<void> {
  if (location.hash === '#room-idle' || location.hash === '#room-busy') {
    const { default: Room } = await import('./RoomPreview.vue');
    createApp(Room, { initialBusy: location.hash === '#room-busy' }).mount('#app');
  } else {
    const [{ default: App }] = await Promise.all([import('./App.vue'), import('./style.css')]);
    createApp(App).mount('#app');
  }
}
void mount();
