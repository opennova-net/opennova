<template>
  <article
    class="overview-area flex flex-col gap-4 rounded-panel border bg-panel p-4"
    :class="BORDER[status.tone]"
    :aria-labelledby="`overview-${area.id}`"
  >
    <div class="flex flex-wrap items-start justify-between gap-2">
      <div class="min-w-0">
        <h3 :id="`overview-${area.id}`" class="text-lg font-semibold text-ink">{{ area.label }}</h3>
        <p class="text-xs text-ink-muted">{{ area.description }}</p>
      </div>
      <span class="overview-area-status shrink-0" :class="toneBadgeClass(status.tone)">{{ status.label }}</span>
    </div>
    <dl class="overview-figures grid grid-cols-1 gap-4 min-[420px]:grid-cols-2">
      <slot />
    </dl>
    <ul class="mt-auto space-y-1 border-t border-border pt-3 text-sm">
      <li v-for="s in area.sections" :key="s.to" class="flex flex-wrap items-baseline gap-x-2">
        <RouterLink :to="s.to" class="font-medium text-accent hover:text-ink-bright">{{ s.label }}</RouterLink>
        <span class="text-xs text-ink-muted">{{ s.description }}</span>
      </li>
    </ul>
  </article>
</template>

<script setup lang="ts">
// One area on the Overview (utils/adminNav.ts): its name and what it holds,
// its state at a glance as a badge in words and colour (the worst of what it
// holds: the page computes it), its key numbers (OverviewFigure, the default
// slot), and a link to each of its pages with what that page holds.
import { RouterLink } from 'vue-router';
import type { AdminArea } from '../utils/adminNav';
import { toneBadgeClass, type Tone } from '../utils/infraView';
import type { AreaStatus } from '../utils/overviewView';

defineProps<{ area: AdminArea; status: AreaStatus }>();

const BORDER: Record<Tone, string> = {
  ok: 'border-border',
  idle: 'border-border',
  warn: 'border-warn/50',
  bad: 'border-danger/60',
};
</script>
