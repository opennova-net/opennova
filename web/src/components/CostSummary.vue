<template>
  <div class="cost-summary space-y-6">
    <div v-if="!costs.available" class="cost-unavailable space-y-1 rounded-panel border border-border bg-panel p-4 text-sm">
      <p class="font-semibold text-ink">AWS costs are not read on this stack.</p>
      <p v-if="costs.unavailable" class="text-ink-muted">{{ costs.unavailable }}.</p>
    </div>
    <p v-else-if="costs.error" class="cost-error rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      AWS costs could not be read: {{ costs.error }}
    </p>

    <template v-else>
      <!-- Where the month stands against the budget, first: a word and a colour. -->
      <div v-if="state" class="cost-budget-state space-y-3 rounded-panel border p-4" :class="STATE_BOX[state.tone]">
        <p class="flex flex-wrap items-center gap-2 text-sm">
          <span :class="toneBadgeClass(state.tone)">{{ state.label }}</span>
          <span class="text-ink">{{ state.detail }}</span>
        </p>
        <div
          class="cost-meter relative h-3 rounded-full bg-hover"
          role="img"
          :aria-label="meterLabel"
        >
          <div
            v-if="meter.forecast !== null"
            class="absolute inset-y-0 left-0 rounded-full opacity-40"
            :class="FILL[state.tone]"
            :style="{ width: `${meter.forecast}%` }"
          ></div>
          <div class="absolute inset-y-0 left-0 rounded-full" :class="FILL[state.tone]" :style="{ width: `${meter.spent}%` }"></div>
          <div class="absolute -inset-y-1 w-0.5 bg-ink" :style="{ left: `calc(${meter.budget}% - 1px)` }" aria-hidden="true"></div>
        </div>
        <p class="flex flex-wrap gap-x-4 gap-y-1 text-xs text-ink-muted" aria-hidden="true">
          <span><span class="font-semibold text-ink">Solid:</span> spent so far</span>
          <span v-if="meter.forecast !== null"><span class="font-semibold text-ink">Faint:</span> the forecast</span>
          <span><span class="font-semibold text-ink">Line:</span> the budget</span>
        </p>
      </div>
      <p v-else-if="costs.budget_error" class="cost-budget-error rounded-panel border border-warn/40 bg-warn/10 p-3 text-sm text-warn">
        The budget could not be read: {{ costs.budget_error }}
      </p>

      <dl class="cost-tiles grid grid-cols-1 gap-3 min-[420px]:grid-cols-3">
        <StatTile label="Spent so far" :value="money(costs.month_to_date_cents)" :detail="period ? `${period}, UTC` : undefined" />
        <StatTile
          :label="month ? `Forecast for ${month}` : 'Forecast for the month'"
          :value="costs.forecast_cents === null ? 'None yet' : money(costs.forecast_cents)"
          :approximate="costs.forecast_cents !== null"
          :detail="forecastDetail"
        />
        <StatTile
          label="Monthly budget"
          :value="costs.budget ? money(costs.budget.limit_cents) : 'None read'"
          :detail="budgetDetail"
        />
      </dl>

      <section class="space-y-3" :aria-labelledby="`${idBase}-services`">
        <component :is="`h${headingLevel}`" :id="`${idBase}-services`" class="text-lg font-semibold text-ink">By AWS service</component>
        <p v-if="costs.by_service.length === 0" class="cost-no-spend text-sm text-ink-muted">
          No spend recorded yet this month. Cost Explorer runs up to a day behind.
        </p>
        <ul v-else class="cost-services space-y-2">
          <li v-for="line in services.top" :key="line.service" class="cost-service text-sm">
            <div class="flex justify-between gap-3">
              <span class="min-w-0 truncate text-ink" :title="line.service">{{ serviceLabel(line.service) }}</span>
              <span class="shrink-0 font-medium tabular-nums text-ink">{{ money(line.cents) }}</span>
            </div>
            <div class="mt-1 h-1.5 rounded-full bg-hover" aria-hidden="true">
              <div class="h-full rounded-full bg-chart-series" :style="{ width: `${barWidth(line.cents)}%` }"></div>
            </div>
          </li>
        </ul>
        <p v-if="services.rest" class="cost-services-rest text-xs text-ink-muted">
          And {{ services.rest.count }} more {{ services.rest.count === 1 ? 'service' : 'services' }}: {{ money(services.rest.cents) }} together.
        </p>
      </section>

      <section v-if="costs.days.length > 1" class="space-y-2" :aria-labelledby="`${idBase}-days`">
        <component :is="`h${headingLevel}`" :id="`${idBase}-days`" class="text-lg font-semibold text-ink">Per day</component>
        <Sparkline :values="dayValues" :label="`Spend per day, ${period}`" :format="money" :width="320" :height="48" />
        <p class="text-xs text-ink-muted">
          Highest {{ money(peakDay.cents) }} on {{ dayWords(peakDay.date) }}; today's is partial.
        </p>
      </section>

      <p class="cost-read text-xs text-ink-muted">
        <template v-if="costs.read_at">Read from AWS <RelativeTime :value="costs.read_at" :now="now" />, at most every 6 hours (each Cost Explorer request costs $0.01). </template>
        Cost Explorer's figures run up to a day behind<template v-if="costs.estimated"> and stay estimates until AWS closes the month</template>.
      </p>
    </template>
  </div>
</template>

<script setup lang="ts">
// The deployment's AWS bill this month (GET /api/admin/costs, fetched by the
// page that shows it): where the month stands against the budget first, as a
// badge in words and colour with a meter (spent, the forecast, the budget's
// line, named beside it), then the month so far, the month's forecast and the
// budget as tiles, the largest AWS services with a bar each and the rest added
// up, the spend per day, and when web read it. Where AWS is not read (the
// local stack) or the read failed, it says so and shows no number.
import { computed } from 'vue';
import RelativeTime from './RelativeTime.vue';
import Sparkline from './charts/Sparkline.vue';
import StatTile from './charts/StatTile.vue';
import type { Costs } from '../types/costs';
import { budgetState, costMoney, monthName, periodLabel, serviceLabel, topServices } from '../utils/costView';
import { toneBadgeClass, type Tone } from '../utils/infraView';

const props = withDefaults(
  defineProps<{
    costs: Costs;
    now: number;
    // The services shown before the rest are added up.
    shown?: number;
    idBase?: string;
    headingLevel?: 2 | 3;
  }>(),
  { shown: 8, idBase: 'costs', headingLevel: 2 },
);

const STATE_BOX: Record<Tone, string> = {
  ok: 'border-online/40 bg-online/5',
  warn: 'border-warn/40 bg-warn/10',
  bad: 'border-danger/40 bg-danger/10',
  idle: 'border-border bg-panel',
};
const FILL: Record<Tone, string> = {
  ok: 'bg-online',
  warn: 'bg-warn',
  bad: 'bg-danger',
  idle: 'bg-ink-muted',
};

function money(cents: number): string {
  return costMoney(cents, props.costs.currency);
}

const state = computed(() => budgetState(props.costs));
const month = computed(() => monthName(props.costs.month?.start ?? null));
const period = computed(() => periodLabel(props.costs));
const services = computed(() => topServices(props.costs.by_service, props.shown));
const largest = computed(() => Math.max(1, ...props.costs.by_service.map((l) => l.cents)));
const dayValues = computed(() => props.costs.days.map((d) => d.cents));
const peakDay = computed(() => props.costs.days.reduce((a, d) => (d.cents > a.cents ? d : a), props.costs.days[0] ?? { date: '', cents: 0 }));

function barWidth(cents: number): number {
  return Math.max(2, Math.min(100, (100 * cents) / largest.value));
}

// "October 3".
function dayWords(date: string): string {
  const d = /^\d{4}-\d{2}-(\d{2})$/.exec(date);
  return d ? `${month.value} ${Number(d[1])}` : date;
}

const forecastDetail = computed(() => {
  if (props.costs.forecast_cents !== null) return "The spend before today and AWS's forecast from today";
  return props.costs.forecast_error ? `AWS gave none: ${props.costs.forecast_error}` : 'AWS gave none';
});

const budgetDetail = computed(() => {
  const b = props.costs.budget;
  if (!b) return props.costs.budget_error ? 'It could not be read' : 'No budget is named here';
  if (b.actual_cents !== null) return `AWS Budgets counts ${money(b.actual_cents)} spent`;
  return b.name;
});

// The meter's three marks as shares of its width: the budget's line inside it
// even when the month passes it.
const meter = computed(() => {
  const s = state.value;
  if (!s) return { spent: 0, forecast: null as number | null, budget: 100 };
  const scale = Math.max(1, s.spentShare, s.forecastShare ?? 0);
  return {
    spent: Math.min(100, (100 * s.spentShare) / scale),
    forecast: s.forecastShare === null ? null : Math.min(100, (100 * s.forecastShare) / scale),
    budget: 100 / scale,
  };
});

const meterLabel = computed(() => {
  const s = state.value;
  const b = props.costs.budget;
  if (!s || !b) return '';
  const parts = [`${money(props.costs.month_to_date_cents)} spent`];
  if (props.costs.forecast_cents !== null) parts.push(`about ${money(props.costs.forecast_cents)} forecast`);
  return `${s.label}: ${parts.join(', ')}, against a budget of ${money(b.limit_cents)}`;
});
</script>
