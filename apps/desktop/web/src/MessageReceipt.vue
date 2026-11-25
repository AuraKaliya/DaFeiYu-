<script setup lang="ts">
import { computed, ref } from 'vue';
import type { StoredMessage } from './message-history';

const props = defineProps<{ record: StoredMessage;
  lookup: (source: string, submission: string, consume: (row: StoredMessage | null | undefined) => void) => void }>();
const state = computed(() => String(props.record.receipt.state));
const checking = ref(false), feedback = ref('');
const labels: Record<string, string> = {
  pending: '已保存，等待投递', dispatching: '正在投递', waiting: '已接收，待带入本轮',
  queued: '已排队，等待下一轮', submitted: '已带入本轮', removed: '已从待发队列移除',
  cancelled: '本轮已停止，补充未带入', deadline: '等待超时，补充未带入', limit: '达到本轮限制，补充未带入',
  closed: '原回合已结束，补充未带入', full: '容量已满，未接纳', stale: '原目标已改变，未接纳',
  rejected: '未接纳', unavailable: '未能确认接纳', unknown: '投递结果待核对',
};
const label = computed(() => {
  if (state.value === 'accepted') return props.record.route === 'enqueue' ? '已从队列进入下一轮' : '';
  if (state.value === 'failed') return props.record.route === 'append' ? '本轮失败，补充未带入' : '投递失败';
  return labels[state.value] ?? '投递结果待核对';
});

// 按数据库原始来源与提交身份复查，只有读回同一持久号才更新核对提示。
function recheck(): void {
  if (checking.value) return;
  checking.value = true; feedback.value = '';
  const original = props.record;
  props.lookup(original.source, original.submission, row => {
    checking.value = false;
    if (!row || row.message !== original.message) feedback.value = '暂未查回原回执';
    else if (row.receipt.state === original.receipt.state) feedback.value = '已核对，结果仍未知';
  });
}
</script>

<template>
  <div v-if="label" class="input-receipt" :data-receipt="state">
    <span>{{ label }}</span>
    <template v-if="label === '投递结果待核对'">
      <button type="button" :disabled="checking" @click="recheck">{{ checking ? '正在核对…' : '复查原回执' }}</button>
      <span v-if="feedback" role="status">{{ feedback }}</span>
    </template>
  </div>
</template>
