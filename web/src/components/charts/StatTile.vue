<template>
  <div
    class="stat-tile relative flex min-w-0 flex-col gap-1 rounded-panel border bg-panel p-4"
    :class="[tone === 'bad' ? 'border-danger/40' : tone === 'warn' ? 'border-warn/40' : 'border-border', to ? 'transition hover:border-accent/60 hover:bg-raised' : '']"
  >
    <dt class="text-xs font-medium text-ink-muted">
      <RouterLink v-if="to" :to="to" class="after:absolute after:inset-0 hover:text-ink-bright">{{ label }}</RouterLink>
      <template v-else>{{ label }}</template>
    </dt>
    <dd class="text-2xl font-semibold" :class="tone === 'bad' ? 'text-danger' : tone === 'warn' ? 'text-warn' : 'text-ink'" :title="approximate ? 'An estimate' : undefined">
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
// are a <dt> and <dd>s. An estimate reads "about". With `to`, the whole tile
// opens the page that holds what it counts; a warn or bad `tone` colours it.
import type { RouteLocationRaw } from 'vue-router';
import { RouterLink } from 'vue-router';
import type { Tone } from '../../utils/tone';

defineProps<{
  label: string;
  value: string;
  detail?: string;
  approximate?: boolean;
  to?: RouteLocationRaw;
  tone?: Tone;
}>();
</script>
