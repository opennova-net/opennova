<template>
  <time :datetime="isoTime(value)" :title="formatUtc(value)">{{ formatRelative(value, now) }}</time>
</template>

<script setup lang="ts">
// A time relative to now ("3 min ago", "in 2 days"), its absolute UTC time on
// hover and in its datetime (utils/format.ts).
import { formatRelative, formatUtc, isoTime } from '../utils/format';

defineProps<{
  // The service's UTC 'YYYY-MM-DD HH:MM:SS', an ISO string or epoch ms.
  value: string | number;
  // The page's clock (ms).
  now: number;
}>();
</script>
