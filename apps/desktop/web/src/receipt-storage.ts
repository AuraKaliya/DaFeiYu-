import { managementRecord } from './management-values.ts';

// 在同一窗口优先使用会话存储，受限时把稳定提交号保存在当前历史项中。
// 步骤：核对该键是否已转存 → 读写原存储位置 → 首次会话写入失败时改用可随刷新保留的历史项。
export function windowReceiptStorage(session: () => Pick<Storage,'getItem'|'setItem'> = () => window.sessionStorage,
    page: Pick<History,'state'|'replaceState'> = window.history): Pick<Storage,'getItem'|'setItem'> {
  // 只借用当前历史项的管理回执字段，其余状态交还原调用者。
  function fallback(): Record<string,unknown> {
    const saved: unknown = page.state?.managementReceipts;
    return managementRecord(saved) ? saved : {};
  }
  return {
    // 每个键从其上次成功写入的位置读回。
    getItem(key) {
      const saved = fallback()[key];
      return typeof saved === 'string' ? saved : session().getItem(key);
    },
    // 只有至少一个可随本页刷新保留的位置写入成功，才返回给业务提交者。
    setItem(key, value) {
      const saved = fallback();
      if (!Object.hasOwn(saved,key)) {
        try { session().setItem(key,value); return; } catch { /* 当前历史项仍可保留原身份。 */ }
      }
      page.replaceState({ ...page.state, managementReceipts: { ...saved, [key]:value } }, '');
    },
  };
}
