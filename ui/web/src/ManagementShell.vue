<script setup lang="ts">
import { ref } from 'vue';

const board = ref<'room' | 'settings'>('room');
const narrowPane = ref<'state' | 'chat'>('chat');
</script>

<template>
  <main class="management-shell">
    <header class="app-bar">
      <h1>大肥鱼管理中心</h1>
      <slot name="status" />
    </header>
    <nav class="board-rail" aria-label="板块">
      <div class="rail-top">
        <button id="board-room" type="button" :aria-current="board === 'room' ? 'page' : undefined" @click="board = 'room'">
          <span class="rail-icon" aria-hidden="true">&#xE80F;</span>房间
        </button>
      </div>
      <div class="rail-bottom">
        <button id="board-settings" type="button" :aria-current="board === 'settings' ? 'page' : undefined" @click="board = 'settings'">
          <span class="rail-icon" aria-hidden="true">&#xE713;</span>设置
        </button>
      </div>
    </nav>
    <section v-show="board === 'room'" class="room-board" aria-label="房间" :data-pane="narrowPane">
      <div class="narrow-switch" role="group" aria-label="房间视图">
        <button type="button" :aria-pressed="narrowPane === 'state'" @click="narrowPane = 'state'">房间状态</button>
        <button type="button" :aria-pressed="narrowPane === 'chat'" @click="narrowPane = 'chat'">聊天</button>
      </div>
      <section class="room-view" aria-label="房间状态"><slot name="room" /></section>
      <section class="chat-pane" aria-label="和大肥鱼聊天"><slot /></section>
    </section>
    <section v-show="board === 'settings'" class="settings-view" aria-labelledby="settings-title">
      <h2 id="settings-title">设置</h2>
      <slot name="settings" />
    </section>
  </main>
</template>

<style scoped>
.management-shell {
  display: grid;
  grid-template: 48px minmax(0, 1fr) / 70px minmax(0, 1fr);
  height: 100dvh; overflow: hidden;
}
.app-bar { grid-column: 1 / -1; display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 0 20px; border-bottom: 1px solid var(--line); background: var(--paper) }
.app-bar h1 { margin: 0; font-size: 15px; font-weight: 650; letter-spacing: .03em }
.board-rail { grid-row: 2; grid-column: 1; display: flex; flex-direction: column; justify-content: space-between; padding: 12px 6px; border-right: 1px solid var(--line); background: #edf2ef }
.rail-top, .rail-bottom { display: flex; gap: 4px }
.rail-top { flex-direction: column }
.rail-bottom { flex-direction: column-reverse }
.board-rail button { display: grid; justify-items: center; gap: 5px; padding: 9px 2px; border: 0; border-radius: 9px; background: transparent; color: var(--muted); font-size: 12px }
.rail-icon { font: 20px/1.2 "Segoe Fluent Icons", "Segoe MDL2 Assets", sans-serif }
.board-rail button:hover { background: #e1eae5 }
.board-rail button[aria-current] { background: #d1e5db; color: #164b39; font-weight: 650 }
.room-board, .settings-view { grid-area: 2 / 2; min-width: 0; min-height: 0 }
.room-board { display: grid; grid-template-columns: minmax(0, 1fr) clamp(300px, 33vw, 460px) }
.room-view, .settings-view { overflow: auto; padding: clamp(18px, 4vw, 50px); scrollbar-gutter: stable }
.settings-view h2 { margin: 0 0 28px }
.chat-pane { display: flex; flex-direction: column; min-width: 0; min-height: 0; padding: 16px; overflow: hidden; border-left: 1px solid var(--line); background: var(--paper) }
.narrow-switch { display: none }
@media (max-width: 680px) {
  .management-shell { grid-template: 40px minmax(0, 1fr) / 48px minmax(0, 1fr) }
  .app-bar { padding: 0 12px }
  .board-rail { padding: 8px 3px }
  .room-board { grid-template: auto minmax(0, 1fr) / minmax(0, 1fr) }
  .narrow-switch { display: flex; gap: 3px; padding: 4px 8px; border-bottom: 1px solid var(--line); background: var(--paper) }
  .narrow-switch button { padding: 5px 12px; border: 0; background: transparent; border-radius: 5px; color: var(--muted); font-size: 13px }
  .narrow-switch button[aria-pressed=true] { color: var(--accent); background: #e5eee8; font-weight: 600 }
  .room-board[data-pane=chat] .room-view, .room-board[data-pane=state] .chat-pane { display: none }
  .chat-pane { padding: 8px; border-left: 0 }
}
</style>
