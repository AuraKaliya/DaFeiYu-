<script setup lang="ts">
import { sourceLabel } from './display.ts';
import type { LineRef } from './trace-file.ts';

defineProps<{ address: LineRef }>();
const emit = defineEmits<{ inspect: [ref: LineRef] }>();
</script>

<template>
  <button type="button" class="source-button mono" @click="emit('inspect', address)">{{ sourceLabel(address) }}</button>
</template>
