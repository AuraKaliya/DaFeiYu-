const key = 'dafeiyu.panel.history-position';
type Position = { conversation: string; message: string; offset: number };

// 刷新只带走逻辑会话、屏幕首条持久号和相对位置，无需保存正文。
export function savePosition(list: HTMLElement | undefined, conversation: string): void {
  if (!list || !conversation || !list.clientHeight) return;
  try {
    if (list.scrollHeight - list.clientHeight - list.scrollTop < 24) { sessionStorage.removeItem(key); return; }
    const top = list.getBoundingClientRect().top;
    const first = Array.from(list.querySelectorAll<HTMLElement>('[data-stored-message]')).find(el => el.getBoundingClientRect().bottom > top);
    if (first) sessionStorage.setItem(key, JSON.stringify({ conversation, message: first.dataset.storedMessage,
      offset: first.getBoundingClientRect().top - top }));
  } catch { /* 历史分页仍会恢复已经读到的边界。 */ }
}

// 仅恢复可以安全定位的持久号，原记录重新排版后再应用偏移。
export function readPosition(): Position | undefined {
  try {
    const saved = JSON.parse(sessionStorage.getItem(key) ?? 'null');
    if (saved && typeof saved.conversation === 'string' && typeof saved.message === 'string' &&
        /^[1-9]\d{0,18}$/.test(saved.message) && Number.isFinite(saved.offset)) return saved;
  } catch { /* 新窗口从最新一页打开。 */ }
}
