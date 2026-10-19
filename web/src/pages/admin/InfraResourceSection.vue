<template>
  <div class="space-y-6">
    <RouterLink to="/admin/infrastructure" class="text-sm text-accent hover:text-ink-bright">Infrastructure</RouterLink>

    <header class="flex flex-col gap-3 sm:flex-row sm:items-end sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">{{ RESOURCE_NAMES[id] }}</h1>
        <p class="text-sm text-ink-muted">Its CloudWatch metrics, {{ range === '48h' ? 'a point a minute over the last 48 hours' : 'a point an hour over the last 400 days' }}.</p>
      </div>
      <fieldset class="min-w-0">
        <legend class="sr-only">Range</legend>
        <div class="inline-flex flex-wrap gap-1 rounded-panel border border-border bg-surface p-1">
          <label v-for="option in RANGES" :key="option" class="relative">
            <input
              :id="`resource-range-${option}`"
              type="radio"
              class="peer sr-only"
              name="resource-range"
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
    </header>

    <p role="alert" class="sr-only">{{ error }}</p>
    <p v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">{{ error }}</p>
    <div v-if="usage === null && !error" class="animate-pulse rounded-panel border border-border bg-panel p-6 text-center text-sm text-ink-muted">
      Loading the metrics…
    </div>

    <template v-if="usage">
      <div v-if="!usage.available" class="resource-unavailable space-y-1 rounded-panel border border-border bg-panel p-4 text-sm">
        <p class="font-semibold text-ink">Not available on the local stack.</p>
        <p v-if="usage.unavailable" class="text-ink-muted">{{ usage.unavailable }}.</p>
      </div>
      <p v-else-if="usage.error" class="resource-error rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">
        CloudWatch could not be read: {{ usage.error }}
      </p>
      <p v-else-if="charts.length === 0" class="text-sm text-ink-muted">CloudWatch holds no point in this range yet.</p>
      <div v-else class="grid gap-4 xl:grid-cols-2" :class="poll.loading.value ? 'opacity-60' : ''">
        <LineChart
          v-for="c in charts"
          :id="`resource-${c.name}`"
          :key="c.name"
          :title="c.view.title"
          :description="`${c.view.description} CloudWatch's ${c.statistic.toLowerCase()} each ${usage.period_seconds === 60 ? 'minute' : 'hour'}.`"
          :series="[c.series]"
          :start="span.start"
          :end="span.end"
          :min-top="c.view.minTop ?? 1"
          :integer="c.view.integer ?? false"
          :format-value="c.view.format"
          gap-text="No point"
          lazy-table
        />
      </div>
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/infrastructure/resources/rds|cache|nlb: one of the deployment's AWS
// resources (GET /api/admin/usage?subject=infra): a chart per CloudWatch
// metric (utils/resourceView.ts names each and its unit) over 48 hours or 400
// days. On the local stack, which reads no AWS, it says so. Polled each minute.
import { computed, ref, watch } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import LineChart from '../../components/charts/LineChart.vue';
import { fetchResourceUsage } from '../../api/usage';
import { usePolling } from '../../composables/usePolling';
import type { ChartSeries } from '../../utils/chartScale';
import type { ResourceId, ResourceUsage, UsageRange } from '../../types/usage';
import { timeMs } from '../../utils/format';
import { RESOURCE_NAMES, metricView, type MetricView } from '../../utils/resourceView';
import { RANGE_LABELS, RANGE_MS } from '../../utils/usageView';

const RANGES: UsageRange[] = ['48h', '400d'];
const POLL_MS = 60_000;

const route = useRoute();
const id = computed<ResourceId>(() => {
  const p = String(route.params.id ?? '');
  return p === 'cache' || p === 'nlb' ? p : 'rds';
});

const range = ref<UsageRange>('48h');
const usage = ref<ResourceUsage | null>(null);
const loadedAt = ref(Date.now());
const error = ref('');

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchResourceUsage(id.value, range.value, signal);
    if (signal.aborted) return false;
    if (result.ok) {
      usage.value = result.value;
      loadedAt.value = Date.now();
      error.value = '';
      return true;
    }
    if (!background || usage.value === null) error.value = result.error.message;
    return false;
  },
  next: () => (usage.value?.available === false ? null : POLL_MS),
});

function setRange(next: UsageRange) {
  if (range.value === next) return;
  range.value = next;
  void poll.refresh();
}

watch(id, () => {
  usage.value = null;
  void poll.refresh();
});

const span = computed(() => ({ start: loadedAt.value - RANGE_MS[range.value], end: loadedAt.value }));

interface Chart {
  name: string;
  view: MetricView;
  statistic: string;
  series: ChartSeries;
}

// A chart per metric, in the reply's order; a Sum of bytes a period shown a second.
const charts = computed<Chart[]>(() => {
  const u = usage.value;
  if (!u || !u.available) return [];
  return Object.entries(u.metrics)
    .filter(([, points]) => points.length > 0)
    .map(([name, points]) => {
      const view = metricView(u.id, name);
      const scale = view.perSecond ? 1 / u.period_seconds : 1;
      return {
        name,
        view,
        statistic: u.statistics[name] ?? 'Average',
        series: { label: view.title, kind: 'main' as const, points: points.map((p) => ({ t: timeMs(p.at), value: p.value * scale })) },
      };
    });
});
</script>
