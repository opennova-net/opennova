<template>
  <figure :id="id" class="min-w-0 space-y-3 rounded-panel border border-border bg-panel p-4" :aria-labelledby="`${id}-title`">
    <figcaption class="space-y-1">
      <p :id="`${id}-title`" class="text-sm font-semibold text-ink">{{ title }}</p>
      <p v-if="description" class="text-xs text-ink-muted">{{ description }}</p>
    </figcaption>

    <!-- The legend, for two series or more: a line key per series. -->
    <ul v-if="series.length > 1" class="flex flex-wrap gap-4 text-xs text-ink-muted" :aria-label="`${title}: legend`">
      <li v-for="s in series" :key="s.label" class="flex items-center gap-2">
        <svg width="16" height="8" aria-hidden="true">
          <line x1="0" y1="4" x2="16" y2="4" :class="s.kind === 'context' ? 'stroke-chart-context' : 'stroke-chart-series'" stroke-width="2" stroke-linecap="round" />
        </svg>
        {{ s.label }}
      </li>
    </ul>

    <!-- The plot. Pointer and arrow keys move the crosshair; the table below
         holds every value, so the hover layer only ever adds to it. -->
    <div
      ref="box"
      class="relative outline-none focus-visible:ring-2 focus-visible:ring-accent"
      :class="loading ? 'opacity-60' : ''"
      tabindex="0"
      role="img"
      :aria-label="`${title}. Use the left and right arrow keys to read each point; the table below has every value.`"
      @keydown="onKey"
      @focus="onFocus"
      @blur="active = -1"
    >
      <svg :width="width" :height="HEIGHT" :viewBox="`0 0 ${width} ${HEIGHT}`" class="block" aria-hidden="true">
        <!-- Gridlines and their labels. -->
        <g>
          <template v-for="tick in yTicks.ticks" :key="tick">
            <line
              :x1="M.left"
              :x2="width - M.right"
              :y1="y(tick)"
              :y2="y(tick)"
              :class="tick === yTicks.domain[0] ? 'stroke-chart-axis' : 'stroke-chart-grid'"
              stroke-width="1"
              shape-rendering="crispEdges"
            />
            <text :x="M.left - 6" :y="y(tick)" dy="0.32em" text-anchor="end" class="fill-ink-muted text-[11px] tabular-nums">
              {{ formatTick(tick) }}
            </text>
          </template>
          <text
            v-for="t in xTicks"
            :key="t"
            :x="x(t)"
            :y="HEIGHT - 6"
            text-anchor="middle"
            class="fill-ink-muted text-[11px] tabular-nums"
          >
            {{ timeLabel(t, end - start) }}
          </text>
        </g>

        <!-- The main series: a wash under a 2px line; the context series a
             thin de-emphasised line. -->
        <path v-if="mainArea" :d="mainArea" class="fill-chart-series/10" />
        <path
          v-for="(d, index) in paths"
          :key="index"
          :d="d.path"
          fill="none"
          :class="d.kind === 'context' ? 'stroke-chart-context' : 'stroke-chart-series'"
          :stroke-width="d.kind === 'context' ? 1 : 2"
          stroke-linejoin="round"
          stroke-linecap="round"
        />

        <!-- The latest value: an end dot with its label. -->
        <template v-if="endPoint && active < 0">
          <circle :cx="endPoint.x" :cy="endPoint.y" r="4" class="fill-chart-series stroke-panel" stroke-width="2" />
          <text
            :x="endPoint.x"
            :y="endPoint.y - 10"
            text-anchor="end"
            class="fill-ink text-[11px] font-semibold tabular-nums"
          >
            {{ formatValue(endPoint.value) }}
          </text>
        </template>

        <!-- The crosshair. -->
        <template v-if="activePoint">
          <line
            :x1="activePoint.x"
            :x2="activePoint.x"
            :y1="M.top"
            :y2="HEIGHT - M.bottom"
            class="stroke-chart-axis"
            stroke-width="1"
            shape-rendering="crispEdges"
          />
          <circle
            v-if="activePoint.y !== null"
            :cx="activePoint.x"
            :cy="activePoint.y"
            r="4"
            class="fill-chart-series stroke-panel"
            stroke-width="2"
          />
        </template>

        <text
          v-if="empty"
          :x="(M.left + width - M.right) / 2"
          :y="(M.top + HEIGHT - M.bottom) / 2"
          text-anchor="middle"
          class="fill-ink-muted text-xs"
        >
          {{ emptyText }}
        </text>

        <!-- The hit area: the whole plot, so the pointer only has to be near a point. -->
        <rect
          :x="M.left"
          :y="M.top"
          :width="Math.max(0, width - M.left - M.right)"
          :height="HEIGHT - M.top - M.bottom"
          fill="transparent"
          @pointermove="onPointer"
          @pointerleave="active = -1"
        />
      </svg>

      <div
        v-if="activePoint"
        class="pointer-events-none absolute top-1 z-10 min-w-[8rem] rounded-panel border border-border-strong bg-surface px-3 py-2 text-xs shadow-lg"
        :style="tooltipStyle"
      >
        <p class="text-ink-muted">{{ formatUtcish(activePoint.t) }}</p>
        <p v-for="row in activeRows" :key="row.label" class="flex items-center gap-2">
          <svg width="12" height="8" aria-hidden="true">
            <line x1="0" y1="4" x2="12" y2="4" :class="row.kind === 'context' ? 'stroke-chart-context' : 'stroke-chart-series'" stroke-width="2" stroke-linecap="round" />
          </svg>
          <span class="font-semibold text-ink">{{ row.text }}</span>
          <span class="text-ink-muted">{{ row.label }}</span>
        </p>
      </div>
    </div>

    <!-- The table view: every value, for anyone. -->
    <details class="text-xs">
      <summary class="cursor-pointer text-accent hover:text-ink-bright">Show the numbers</summary>
      <div class="mt-2 max-h-72 overflow-auto rounded-panel border border-border">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">{{ title }}</caption>
          <thead>
            <tr class="bg-raised text-left text-ink-muted">
              <th scope="col" class="px-3 py-1.5 font-semibold">Time</th>
              <th v-for="s in series" :key="s.label" scope="col" class="px-3 py-1.5 text-right font-semibold">{{ s.label }}</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-ink">
            <tr v-for="(t, index) in times" :key="t">
              <th scope="row" class="whitespace-nowrap px-3 py-1 text-left font-normal">{{ formatUtcish(t) }}</th>
              <td v-for="s in series" :key="s.label" class="px-3 py-1 text-right tabular-nums">
                {{ cellText(s.points[index]?.value ?? null) }}
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </details>
  </figure>
</template>

<script setup lang="ts">
// A line chart over time, hand-written SVG (the dataviz skill's marks: a 2px
// line in the series hue with a 10% wash under it, a 1px context line in the
// de-emphasis gray, hairline grid and baseline, an end dot with a 2px surface
// ring and the latest value beside it). The first series is the main one;
// others are context (a limit beside it). A value of null is a gap (the
// server was not running). The viewBox follows the box's measured width, so
// text keeps its size on a phone. The crosshair follows the pointer or the
// arrow keys; the table under it ("Show the numbers") holds every value.
import { computed, onMounted, onUnmounted, ref } from 'vue';
import {
  areaPath,
  linePath,
  nearestIndex,
  niceTicks,
  scaleLinear,
  timeLabel,
  timeTicks,
  type ChartSeries,
} from '../../utils/chartScale';
import { formatUtc } from '../../utils/format';

const props = withDefaults(
  defineProps<{
    id: string;
    title: string;
    description?: string;
    series: ChartSeries[];
    // The x domain, epoch ms.
    start: number;
    end: number;
    formatValue?: (value: number) => string;
    // Whole-number ticks (counts).
    integer?: boolean;
    // The y domain's least top (a player count's chart reaches 1 at least).
    minTop?: number;
    loading?: boolean;
    emptyText?: string;
    // What a gap reads as in the table.
    gapText?: string;
  }>(),
  {
    description: '',
    formatValue: (value: number) => String(value),
    integer: false,
    minTop: 1,
    loading: false,
    emptyText: 'Nothing recorded in this range yet.',
    gapText: 'Not running',
  },
);

const HEIGHT = 200;
const M = { top: 16, right: 16, bottom: 24, left: 44 };

const box = ref<HTMLElement | null>(null);
const width = ref(640);
const active = ref(-1);
let observer: ResizeObserver | null = null;

onMounted(() => {
  const el = box.value;
  if (!el) return;
  if (el.clientWidth > 0) width.value = el.clientWidth;
  if (typeof ResizeObserver !== 'undefined') {
    observer = new ResizeObserver((entries) => {
      const w = Math.round(entries[0]?.contentRect.width ?? 0);
      if (w > 0) width.value = w;
    });
    observer.observe(el);
  }
});

onUnmounted(() => observer?.disconnect());

const main = computed(() => props.series[0] ?? null);
const times = computed(() => main.value?.points.map((p) => p.t) ?? []);
const values = computed(() =>
  props.series.flatMap((s) => s.points.map((p) => p.value)).filter((v): v is number => v !== null && Number.isFinite(v)),
);
const empty = computed(() => (main.value?.points ?? []).every((p) => p.value === null));

const yTicks = computed(() => niceTicks(0, Math.max(props.minTop, ...values.value), 4, props.integer));
const x = computed(() => scaleLinear([props.start, props.end], [M.left, width.value - M.right]));
const y = computed(() => scaleLinear(yTicks.value.domain, [HEIGHT - M.bottom, M.top]));
const xTicks = computed(() => timeTicks(props.start, props.end, Math.max(2, Math.floor((width.value - M.left - M.right) / 90))));

function plotted(s: ChartSeries) {
  return s.points.map((p) => ({ x: x.value(p.t), y: p.value === null ? null : y.value(p.value) }));
}

const paths = computed(() =>
  // The context first, so the main line draws over it.
  [...props.series]
    .sort((a, b) => (a.kind === b.kind ? 0 : a.kind === 'context' ? -1 : 1))
    .map((s) => ({ kind: s.kind, path: linePath(plotted(s)) })),
);
const mainArea = computed(() => (main.value ? areaPath(plotted(main.value), HEIGHT - M.bottom) : ''));

const endPoint = computed(() => {
  const points = main.value?.points ?? [];
  for (let i = points.length - 1; i >= 0; i--) {
    const v = points[i].value;
    if (v !== null) return { x: x.value(points[i].t), y: y.value(v), value: v };
  }
  return null;
});

const activePoint = computed(() => {
  const p = main.value?.points[active.value];
  if (!p) return null;
  return { t: p.t, x: x.value(p.t), y: p.value === null ? null : y.value(p.value) };
});

const activeRows = computed(() =>
  props.series.map((s) => {
    const v = s.points[active.value]?.value ?? null;
    return { label: s.label, kind: s.kind, text: cellText(v) };
  }),
);

const tooltipStyle = computed(() => {
  const p = activePoint.value;
  if (!p) return {};
  // Beside the crosshair, on whichever side has room.
  return p.x > width.value / 2 ? { right: `${width.value - p.x + 10}px` } : { left: `${p.x + 10}px` };
});

function cellText(v: number | null): string {
  return v === null ? props.gapText : props.formatValue(v);
}

function formatTick(v: number): string {
  return props.formatValue(v);
}

function formatUtcish(t: number): string {
  return formatUtc(t);
}

function onPointer(event: PointerEvent) {
  const el = box.value;
  if (!el) return;
  const rect = el.getBoundingClientRect();
  const px = ((event.clientX - rect.left) / (rect.width || width.value)) * width.value;
  active.value = nearestIndex(times.value.map((t) => x.value(t)), px);
}

function onFocus() {
  if (active.value < 0 && times.value.length) active.value = times.value.length - 1;
}

function onKey(event: KeyboardEvent) {
  const n = times.value.length;
  if (n === 0) return;
  let next = active.value < 0 ? n - 1 : active.value;
  switch (event.key) {
    case 'ArrowLeft':
      next = Math.max(0, next - 1);
      break;
    case 'ArrowRight':
      next = Math.min(n - 1, next + 1);
      break;
    case 'Home':
      next = 0;
      break;
    case 'End':
      next = n - 1;
      break;
    case 'Escape':
      active.value = -1;
      return;
    default:
      return;
  }
  event.preventDefault();
  active.value = next;
}
</script>
