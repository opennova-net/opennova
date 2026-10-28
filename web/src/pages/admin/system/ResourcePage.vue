<template>
  <div class="space-y-6">
    <PageHeader :title="RESOURCE_NAMES[id]" :crumbs="CRUMBS">
      <template #actions>
        <RangePicker v-model="range" base-id="resource-range" legend="Range" :options="RANGE_OPTIONS" />
      </template>
    </PageHeader>

    <LiveNotices :error="error" />
    <p v-if="usage === null && !error" class="text-sm text-ink-muted">Loading…</p>

    <template v-if="usage">
      <Notice v-if="!usage.available" class="resource-unavailable" :title="usage.unavailable ?? undefined">AWS is not read on this stack.</Notice>
      <Notice v-else-if="usage.error" tone="bad" class="resource-error">CloudWatch could not be read: {{ usage.error }}</Notice>
      <p v-else-if="charts.length === 0" class="text-sm text-ink-muted">No points in this range yet.</p>
      <div v-else class="grid gap-4 xl:grid-cols-2" :class="poll.loading.value ? 'opacity-60' : ''">
        <LineChart
          v-for="c in charts"
          :id="`resource-${c.name}`"
          :key="c.name"
          :title="c.view.title"
          :description="`${c.view.description} ${c.statistic} a ${usage.period_seconds === 60 ? 'minute' : 'hour'}.`"
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
// /admin/resources/rds|cache|nlb: one of the deployment's AWS resources, a
// chart per CloudWatch metric over 48 hours or 400 days. On the local stack,
// which reads no AWS, it says so. Polled each minute.
import { computed, ref, watch } from 'vue';
import { useRoute } from 'vue-router';
import LineChart from '../../../components/charts/LineChart.vue';
import RangePicker from '../../../components/charts/RangePicker.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import Notice from '../../../components/ui/Notice.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import { fetchResourceUsage } from '../../../api/usage';
import { usePolling } from '../../../composables/usePolling';
import type { ChartSeries } from '../../../utils/chartScale';
import type { ResourceId, ResourceUsage, UsageRange } from '../../../types/usage';
import type { Crumb } from '../../../utils/adminNav';
import { timeMs } from '../../../utils/format';
import { RESOURCE_NAMES, metricView, type MetricView } from '../../../utils/resourceView';
import { RANGE_LABELS, RANGE_MS } from '../../../utils/usageView';

const RANGE_OPTIONS: { value: UsageRange; label: string }[] = (['48h', '400d'] as const).map((value) => ({ value, label: RANGE_LABELS[value] }));
const POLL_MS = 60_000;
const CRUMBS: Crumb[] = [
  { label: 'System', to: '/admin/system' },
  { label: 'Infrastructure', to: '/admin/system/infrastructure' },
];

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

watch(range, () => {
  void poll.refresh();
});

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
