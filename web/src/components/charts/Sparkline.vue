<template>
  <svg
    :width="width"
    :height="height"
    :viewBox="`0 0 ${width} ${height}`"
    role="img"
    :aria-label="summary"
    class="block max-w-full overflow-visible"
  >
    <title>{{ summary }}</title>
    <path :d="path" fill="none" class="stroke-chart-context" stroke-width="1.5" stroke-linejoin="round" stroke-linecap="round" />
    <circle
      v-if="last"
      :cx="last.x"
      :cy="last.y"
      r="3"
      class="fill-chart-series stroke-panel"
      stroke-width="2"
    />
  </svg>
</template>

<script setup lang="ts">
// A small trend line for a stat tile (the dataviz skill's sparkline): the
// series in the de-emphasis hue, its latest value as a dot in the series hue.
// A gap where a value is missing. Its label says what it shows in words.
import { computed } from 'vue';
import { linePath, niceTicks, scaleLinear } from '../../utils/chartScale';

const props = withDefaults(
  defineProps<{
    values: (number | null)[];
    // What the line shows, for its label: "Players online".
    label: string;
    format?: (value: number) => string;
    width?: number;
    height?: number;
  }>(),
  { width: 120, height: 32, format: (value: number) => String(value) },
);

const PAD = 4;

const shown = computed(() => props.values.filter((v): v is number => v !== null && Number.isFinite(v)));

const plotted = computed(() => {
  const n = props.values.length;
  const max = shown.value.length ? Math.max(...shown.value) : 0;
  const { domain } = niceTicks(0, max, 2);
  const x = scaleLinear([0, Math.max(1, n - 1)], [PAD, props.width - PAD]);
  const y = scaleLinear(domain, [props.height - PAD, PAD]);
  return props.values.map((v, i) => ({ x: x(i), y: v === null || !Number.isFinite(v) ? null : y(v) }));
});

const path = computed(() => linePath(plotted.value));

const last = computed(() => {
  const p = plotted.value[plotted.value.length - 1];
  return p && p.y !== null ? { x: p.x, y: p.y } : null;
});

const summary = computed(() => {
  if (shown.value.length === 0) return `${props.label}: no data yet`;
  const latest = props.values[props.values.length - 1];
  const peak = Math.max(...shown.value);
  const now = latest === null || latest === undefined ? 'none now' : `${props.format(latest)} now`;
  return `${props.label}: ${now}, peak ${props.format(peak)}`;
});
</script>
