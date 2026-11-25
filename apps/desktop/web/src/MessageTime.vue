<script setup lang="ts">
import type { StoredMessage } from './message-history';
defineProps<{ record?: StoredMessage }>();
const localTime = new Intl.DateTimeFormat(undefined, { dateStyle: 'short', timeStyle: 'short' });
</script>

<template>
  <time v-if="record" :datetime="new Date(record.utcMs).toISOString()">{{ localTime.format(record.utcMs) }}</time>
</template>
