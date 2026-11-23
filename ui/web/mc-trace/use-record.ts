import { shallowRef, watch } from 'vue';
import { loadRecord } from './viewer-state.ts';
import type { LoadedTrace } from './viewer-state.ts';
import type { LineRef } from './trace-file.ts';

// 读取详情所选的原行，快速切换时丢弃旧选择的结果。
// 步骤：复位上一次选择 → 异步按字节读取 → 只提交当前详情。
export function useRecord(source: () => LoadedTrace, address: () => LineRef | undefined) {
  const value = shallowRef<Awaited<ReturnType<typeof loadRecord>>>();
  const error = shallowRef('');
  const pending = shallowRef(false);
  watch([source, address], async ([file, ref], _prior, cleanup) => {
    let valid = true;
    cleanup(() => { valid = false; });
    value.value = undefined;
    error.value = '';
    pending.value = ref !== undefined;
    if (!ref) return;

    try {
      const result = await loadRecord(file, ref);
      if (valid) value.value = result;
    } catch (failure) {
      if (valid) error.value = failure instanceof Error ? failure.message : '读取原记录失败';
    } finally {
      if (valid) pending.value = false;
    }
  }, { immediate: true });
  return { value, error, pending };
}
