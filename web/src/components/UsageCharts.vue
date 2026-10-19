<template>
  <section class="space-y-4" :aria-labelledby="`${idBase}-heading`">
    <div class="flex flex-col gap-3 sm:flex-row sm:items-end sm:justify-between">
      <div>
        <component :is="`h${headingLevel}`" :id="`${idBase}-heading`" class="text-lg font-semibold text-ink">{{ heading }}</component>
        <p class="text-sm text-ink-muted">CPU, memory and network, {{ rangeWords(range) }}.</p>
      </div>
      <!-- The range: a radio group, so the arrow keys move it. -->
      <fieldset class="min-w-0">
        <legend class="sr-only">Usage range</legend>
        <div class="inline-flex flex-wrap gap-1 rounded-panel border border-border bg-surface p-1">
          <label v-for="option in RANGES" :key="option" class="relative">
            <input
              :id="`${idBase}-range-${option}`"
              type="radio"
              class="peer sr-only"
              :name="`${idBase}-range`"
              :value="option"
              :checked="range === option"
              @change="setRange(option)"
            />
            <span
              class="block cursor-pointer rounded-control px-3 py-1.5 text-sm text-ink-muted transition hover:text-ink peer-checked:bg-raised peer-checked:font-semibold peer-checked:text-ink peer-focus-visible:ring-2 peer-focus-visible:ring-accent"
            >
              {{ RANGE_LABELS[option] }}
            </span>
          </label>
        </div>
      </fieldset>
    </div>

    <p role="alert" class="sr-only">{{ error }}</p>
    <p v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">{{ error }}</p>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this keeps trying.</p>

    <div v-if="usage === null && !error" class="animate-pulse rounded-panel border border-border bg-panel p-6 text-center text-sm text-ink-muted">
      Loading the usage…
    </div>

    <template v-if="usage">
      <!-- What it uses now, in words. -->
      <p class="usage-now rounded-panel border p-3 text-sm" :class="usage.now ? 'border-border bg-panel text-ink' : 'border-warn/40 bg-warn/10 text-warn'">
        <span class="font-semibold">Now.</span> {{ nowLine }}
      </p>

      <div class="grid gap-4 transition-opacity xl:grid-cols-2" :class="poll.loading.value ? 'opacity-60' : ''">
        <LineChart
          :id="`${idBase}-cpu`"
          title="CPU"
          :description="cpuDescription"
          :series="cpuSeries"
          :start="span.start"
          :end="span.end"
          :min-top="100"
          :format-value="formatPercent"
          gap-text="No sample"
          lazy-table
        />
        <LineChart
          :id="`${idBase}-memory`"
          title="Memory"
          :description="memoryDescription"
          :series="memorySeries"
          :start="span.start"
          :end="span.end"
          :min-top="usage.limit.memory_bytes ?? 1"
          :format-value="formatMemory"
          gap-text="No sample"
          lazy-table
        />
        <LineChart
          :id="`${idBase}-network`"
          title="Network"
          description="Bytes a second sent out (the line) and taken in (the thin line)."
          :series="networkSeries"
          :start="span.start"
          :end="span.end"
          :format-value="formatByteRate"
          gap-text="No sample"
          lazy-table
        />
        <template v-if="subject === 'server'">
          <LineChart
            :id="`${idBase}-ticks`"
            title="Tick time, 99th percentile"
            :description="tickDescription"
            :series="tickSeries"
            :start="span.start"
            :end="span.end"
            :min-top="20"
            :format-value="formatFrameTime"
            gap-text="No sample"
            lazy-table
          />
          <LineChart
            :id="`${idBase}-late`"
            title="Late ticks"
            :description="lateDescription"
            :series="lateSeries"
            :start="span.start"
            :end="span.end"
            integer
            :format-value="formatCount"
            gap-text="No sample"
            lazy-table
          />
        </template>
        <LineChart
          v-if="subject === 'machine'"
          :id="`${idBase}-disk`"
          title="Disk free"
          :description="diskDescription"
          :series="diskSeries"
          :start="span.start"
          :end="span.end"
          :format-value="formatBytes"
          gap-text="No sample"
          lazy-table
        />
      </div>
    </template>
  </section>
</template>

<script setup lang="ts">
// The same usage charts for every subject (a hosted server, a machine, a
// program or one of its tasks; GET /api/admin/usage): what it uses now in one
// line of words, then CPU in percent of its CPUs, memory against its limit
// and network in and out, at a point a minute over 48 hours or an hour over
// 400 days; a server's tick health (its frames' 99th percentile, its late
// ticks) and a machine's free disk beside them. A gap is a stretch with no
// sample. Polled each minute (every 5 while the 400 days show) while
// `follow` holds.
import { computed, ref, watch } from 'vue';
import LineChart from './charts/LineChart.vue';
import { fetchUsage } from '../api/usage';
import { usePolling } from '../composables/usePolling';
import type { Usage, UsageRange, UsageSubject } from '../types/usage';
import { formatByteRate, formatBytes, formatCount, formatCpus, formatFrameTime, formatMemory, formatPercent } from '../utils/format';
import {
  RANGE_LABELS,
  RANGE_MS,
  limitSeries,
  rangeWords,
  usageExtras,
  usageLine,
  usageNow,
  usageSeries,
  usageTimeline,
} from '../utils/usageView';

const props = withDefaults(
  defineProps<{
    // The base of the charts' ids.
    idBase: string;
    subject: UsageSubject;
    // The server's or machine's row id, or the program's name.
    id: string | number;
    // A program's one task.
    task?: string;
    // What the now line names ("web", "server 12").
    name: string;
    heading?: string;
    headingLevel?: 2 | 3;
    follow?: boolean;
  }>(),
  { task: undefined, heading: 'Usage', headingLevel: 3, follow: true },
);

const RANGES: UsageRange[] = ['48h', '400d'];
const MINUTE_POLL_MS = 60_000;
const HOUR_POLL_MS = 5 * 60_000;
// A frame's time at 62.5 Hz: past it, the frame overran its tick.
const TICK_MS = 16;

const range = ref<UsageRange>('48h');
const usage = ref<Usage | null>(null);
const loadedAt = ref(Date.now());
const error = ref('');
const pollTrouble = ref(false);

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchUsage(props.subject, props.id, range.value, { task: props.task, signal });
    if (signal.aborted) return false;
    if (result.ok) {
      usage.value = result.value;
      loadedAt.value = Date.now();
      error.value = '';
      pollTrouble.value = false;
      return true;
    }
    if (background && usage.value !== null) pollTrouble.value = true;
    else error.value = result.error.message;
    return false;
  },
  next: () => (props.follow ? (range.value === '48h' ? MINUTE_POLL_MS : HOUR_POLL_MS) : null),
});

function setRange(next: UsageRange) {
  if (range.value === next) return;
  range.value = next;
  void poll.refresh();
}

// Another subject in the same place: start over.
watch(
  () => [props.subject, props.id, props.task],
  () => {
    usage.value = null;
    error.value = '';
    void poll.refresh();
  },
);

const span = computed(() => ({ start: loadedAt.value - RANGE_MS[range.value], end: loadedAt.value }));
const timeline = computed(() => (usage.value ? usageTimeline(usage.value) : []));

const nowLine = computed(() => {
  const u = usage.value;
  if (!u) return '';
  return usageLine(props.name, usageNow(u), usageExtras(u));
});

const cpuDescription = computed(() => {
  const cpu = usage.value?.limit.cpu ?? null;
  return cpu !== null ? `In percent of its ${formatCpus(cpu)}.` : "In percent of the machine's CPUs.";
});
const cpuSeries = computed(() => [usageSeries(timeline.value, 'CPU', 'main', (p) => p.cpu_percent)]);

const memoryDescription = computed(() => {
  const limit = usage.value?.limit.memory_bytes ?? null;
  const measure = range.value === '48h' ? 'Used' : 'The most used in the hour';
  return limit !== null ? `${measure}, against its limit of ${formatMemory(limit)} (the thin line).` : `${measure}.`;
});
const memorySeries = computed(() => {
  const limit = usage.value?.limit.memory_bytes ?? null;
  const series = [usageSeries(timeline.value, 'Used', 'main', (p) => p.memory_bytes)];
  if (limit !== null) series.push(limitSeries(timeline.value, 'Limit', limit));
  return series;
});

const networkSeries = computed(() => [
  usageSeries(timeline.value, 'Out', 'main', (p) => p.net_out_bps),
  usageSeries(timeline.value, 'In', 'context', (p) => p.net_in_bps),
]);

const tickDescription = computed(
  () =>
    `How long the slowest 1 % of its frames took${range.value === '48h' ? '' : ' (the worst minute of the hour)'}; ` +
    `past the thin line, ${TICK_MS} ms at 62.5 Hz, a frame overran its tick.`,
);
const tickSeries = computed(() => [
  usageSeries(timeline.value, '99th percentile', 'main', (p) => p.tick_p99_ms),
  limitSeries(timeline.value, 'One tick', TICK_MS),
]);
const lateDescription = computed(
  () =>
    `Frames that started a whole tick behind${range.value === '48h' ? ', each minute' : ', each hour'}: ` +
    'the machine could not keep up.',
);
const lateSeries = computed(() => [usageSeries(timeline.value, 'Late ticks', 'main', (p) => p.late_ticks)]);

const diskDescription = computed(() => {
  const total = usage.value?.disk_total_bytes ?? null;
  return total !== null ? `Free on its data disk of ${formatBytes(total)} (the thin line).` : 'Free on its data disk.';
});
const diskSeries = computed(() => {
  const total = usage.value?.disk_total_bytes ?? null;
  const series = [usageSeries(timeline.value, 'Free', 'main', (p) => p.disk_free_bytes)];
  if (total !== null) series.push(limitSeries(timeline.value, 'Size', total));
  return series;
});
</script>
