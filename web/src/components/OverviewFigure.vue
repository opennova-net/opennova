<template>
  <div class="overview-figure flex min-w-0 flex-col gap-0.5">
    <dt class="text-xs font-medium text-ink-muted">{{ label }}</dt>
    <dd class="text-xl font-semibold" :class="tone ? toneTextClass(tone) : 'text-ink'">
      <span v-if="approximate" class="text-sm font-normal text-ink-muted">about </span>{{ value }}
    </dd>
    <dd v-if="detail" class="text-xs text-ink-muted">{{ detail }}</dd>
    <dd v-if="$slots.trend" class="pt-1"><slot name="trend" /></dd>
  </div>
</template>

<script setup lang="ts">
// One key number in an Overview area (OverviewArea's <dl>): its label, its
// value (in words where it is a state: "Maintenance", "2 past due"), coloured
// by `tone` only beside those words, a line of detail and an optional trend.
import { toneTextClass, type Tone } from '../utils/infraView';

defineProps<{
  label: string;
  value: string;
  detail?: string;
  tone?: Tone;
  approximate?: boolean;
}>();
</script>
