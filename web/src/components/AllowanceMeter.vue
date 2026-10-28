<template>
  <div class="allowance-meter min-w-0 space-y-1" :data-severity="severity">
    <div class="flex min-w-0 items-center gap-3">
      <p :id="id" class="shrink-0 text-sm text-ink">
        <span class="font-semibold">{{ shown(used) }}</span>
        <span class="text-ink-muted"> of </span>
        <span class="font-semibold">{{ shown(max) }}</span>
        {{ max === 1 ? singular : plural }}
      </p>
      <!-- The sentence says it all, so the bar is decoration to a screen
           reader: a segment per place, a continuous bar past SEGMENTS_MAX. -->
      <div v-if="segmented" aria-hidden="true" class="flex h-1.5 min-w-16 flex-1 gap-0.5">
        <span
          v-for="index in max"
          :key="index"
          class="h-full flex-1 first:rounded-l-full last:rounded-r-full"
          :class="index <= used ? fillClass : trackClass"
        ></span>
      </div>
      <div v-else aria-hidden="true" class="h-1.5 min-w-16 flex-1 overflow-hidden rounded-full" :class="trackClass">
        <div class="h-full rounded-full" :class="fillClass" :style="{ width: `${fraction * 100}%` }"></div>
      </div>
    </div>
    <p v-if="severity === 'over'" class="text-xs text-danger">{{ overText }}</p>
  </div>
</template>

<script setup lang="ts">
// "2 of 3 shared servers" as one row with its meter: the series colour while
// there is room, warning once full, danger past the limit. `format` shows the
// counts another way (bytes).
import { computed } from 'vue';

const props = withDefaults(
  defineProps<{
    used: number;
    max: number;
    // The sentence's id, for an aria-describedby elsewhere.
    id: string;
    singular?: string;
    plural?: string;
    format?: (n: number) => string;
    // The line shown past the limit.
    overText?: string;
  }>(),
  {
    singular: 'server',
    plural: 'servers',
    format: undefined,
    overText: 'More are live than you may run: the newest are being stopped.',
  },
);

const SEGMENTS_MAX = 16;

const shown = (n: number) => (props.format ? props.format(n) : String(n));

const severity = computed<'room' | 'full' | 'over'>(() => {
  if (props.used > props.max) return 'over';
  if (props.used === props.max) return 'full';
  return 'room';
});

const segmented = computed(() => !props.format && props.max > 0 && props.max <= SEGMENTS_MAX);
const fraction = computed(() => (props.max > 0 ? Math.min(1, props.used / props.max) : 0));

const fillClass = computed(() => {
  switch (severity.value) {
    case 'over':
      return 'bg-danger';
    case 'full':
      return 'bg-warn';
    default:
      return 'bg-chart-series';
  }
});

const trackClass = computed(() => {
  switch (severity.value) {
    case 'over':
      return 'bg-danger/20';
    case 'full':
      return 'bg-warn/20';
    default:
      return 'bg-chart-series/20';
  }
});
</script>
