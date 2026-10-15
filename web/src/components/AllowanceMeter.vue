<template>
  <div class="space-y-2" :data-severity="severity">
    <p :id="id" class="text-sm text-ink">
      <span class="text-2xl font-semibold text-ink">{{ used }}</span>
      <span class="text-ink-muted"> of </span>
      <span class="font-semibold">{{ max }}</span>
      {{ max === 1 ? singular : plural }} used
    </p>
    <!-- The sentence above says it all, so the bar is decoration to a screen
         reader. One segment per place, with a 2px gap; a continuous bar past
         SEGMENTS_MAX places. -->
    <div v-if="segmented" aria-hidden="true" class="flex h-2 gap-0.5">
      <span
        v-for="index in max"
        :key="index"
        class="h-full flex-1 first:rounded-l-full last:rounded-r-full"
        :class="index <= used ? fillClass : trackClass"
      ></span>
    </div>
    <div v-else aria-hidden="true" class="h-2 overflow-hidden rounded-full" :class="trackClass">
      <div class="h-full rounded-full" :class="fillClass" :style="{ width: `${fraction * 100}%` }"></div>
    </div>
    <p v-if="severity === 'over'" class="text-xs text-danger">
      More are live than you may run at once: the newest are being stopped.
    </p>
  </div>
</template>

<script setup lang="ts">
// "2 of 3 servers used": a count against its limit as a meter. The fill
// carries the state: the series colour while there is room, warning once
// every place is used, danger past the limit (after a support tier moved
// down); the track is the same hue's dim step.
import { computed } from 'vue';

const props = withDefaults(
  defineProps<{
    used: number;
    max: number;
    // The sentence's id, for an aria-describedby elsewhere.
    id: string;
    singular?: string;
    plural?: string;
  }>(),
  { singular: 'server', plural: 'servers' },
);

const SEGMENTS_MAX = 16;

const severity = computed<'room' | 'full' | 'over'>(() => {
  if (props.used > props.max) return 'over';
  if (props.used === props.max) return 'full';
  return 'room';
});

const segmented = computed(() => props.max > 0 && props.max <= SEGMENTS_MAX);
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
