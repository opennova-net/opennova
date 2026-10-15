<template>
  <div class="flex min-w-0 flex-col gap-1 rounded-panel border border-border bg-panel p-4">
    <dt class="text-xs font-medium text-ink-muted">{{ label }}</dt>
    <dd class="text-2xl font-semibold text-ink" :title="approximate ? 'An estimate' : undefined">
      <span v-if="approximate" class="text-base font-normal text-ink-muted">about </span>{{ value }}
    </dd>
    <dd v-if="detail" class="text-xs text-ink-muted">{{ detail }}</dd>
    <dd v-if="$slots.trend" class="pt-1"><slot name="trend" /></dd>
  </div>
</template>

<script setup lang="ts">
// One headline number (the dataviz skill's stat tile): its label in sentence
// case, its value in the page's sans with proportional figures, a line of
// detail, and an optional trend (a Sparkline). It sits in a <dl>, so its parts
// are a <dt> and <dd>s. An estimate reads "about".
defineProps<{
  label: string;
  value: string;
  detail?: string;
  approximate?: boolean;
}>();
</script>
