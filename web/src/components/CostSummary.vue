<template>
  <div class="cost-summary space-y-6">
    <p v-if="multipleAccounts" class="cost-scope text-sm font-semibold text-ink">{{ scope }} · Combined AWS costs</p>
    <div v-if="!costs.available" class="cost-unavailable space-y-1 rounded-panel border border-border bg-panel p-4 text-sm">
      <p class="font-semibold text-ink">AWS costs are not read on this stack.</p>
      <p v-if="costs.unavailable" class="text-ink-muted">{{ costs.unavailable }}.</p>
    </div>
    <p v-else-if="costs.error" class="cost-error rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      {{ multipleAccounts ? 'Combined total unavailable' : 'AWS costs could not be read' }}: {{ costs.error }}
    </p>

    <template v-else>
      <!-- Where the month stands against the budget, first: a word and a colour. -->
      <div v-if="state" class="cost-budget-state space-y-3 rounded-panel border p-4" :class="STATE_BOX[state.tone]">
        <p class="flex flex-wrap items-center gap-2 text-sm">
          <span :class="badgeClass(state.tone)">{{ state.label }}</span>
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
      <p v-else-if="!multipleAccounts && costs.budget_error" class="cost-budget-error rounded-panel border border-warn/40 bg-warn/10 p-3 text-sm text-warn">
        The budget could not be read: {{ costs.budget_error }}
      </p>
      <p v-else-if="!multipleAccounts && costs.budget && costs.budget.currency.toUpperCase() !== costs.currency.toUpperCase()" class="text-sm text-ink-muted">
        Spending and budget use different currencies, so they cannot be compared.
      </p>

      <dl class="cost-tiles grid grid-cols-1 gap-3" :class="multipleAccounts ? 'min-[420px]:grid-cols-2' : 'min-[420px]:grid-cols-3'">
        <StatTile label="Spent so far" :value="money(costs.month_to_date_cents)" :detail="period ? `${period}, UTC` : undefined" />
        <StatTile
          :label="month ? `Forecast for ${month}` : 'Forecast for the month'"
          :value="costs.forecast_cents === null ? 'None yet' : money(costs.forecast_cents)"
          :approximate="costs.forecast_cents !== null"
          :detail="forecastDetail"
        />
        <StatTile
          v-if="!multipleAccounts"
          label="Monthly budget"
          :value="costs.budget ? costMoney(costs.budget.limit_cents, costs.budget.currency) : 'None read'"
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

    </template>

    <section v-if="multipleAccounts && costs.available" class="cost-accounts space-y-3" :aria-labelledby="`${idBase}-accounts`">
      <component :is="`h${headingLevel}`" :id="`${idBase}-accounts`" class="text-lg font-semibold text-ink">By account</component>
      <p class="text-sm text-ink-muted">Each account is compared with its own monthly budget.</p>
      <article v-for="account in accounts" :key="account.id" class="cost-account space-y-3 rounded-panel border border-border bg-panel p-4">
        <component :is="`h${headingLevel + 1}`" class="font-semibold text-ink">{{ account.label }}</component>
        <p v-if="account.error" class="cost-account-error text-sm text-danger">Costs could not be read: {{ account.error }}</p>
        <template v-else>
          <p v-if="account.state" class="cost-account-budget-state flex flex-wrap items-center gap-2 text-sm">
            <span :class="badgeClass(account.state.tone)">{{ account.state.label }}</span>
            <span class="text-ink">{{ account.state.detail }}</span>
          </p>
          <p v-else-if="account.budget_error" class="cost-account-budget-error text-sm text-warn">The budget could not be read: {{ account.budget_error }}</p>
          <p v-else-if="account.budget && account.budget.currency.toUpperCase() !== account.currency.toUpperCase()" class="text-sm text-ink-muted">
            Spending and budget use different currencies, so they cannot be compared.
          </p>
          <dl class="grid grid-cols-1 gap-3 min-[420px]:grid-cols-3">
            <StatTile label="Spent so far" :value="costMoney(account.month_to_date_cents, account.currency)" />
            <StatTile
              :label="month ? `Forecast for ${month}` : 'Forecast for the month'"
              :value="account.forecast_cents === null ? 'None yet' : costMoney(account.forecast_cents, account.currency)"
              :approximate="account.forecast_cents !== null"
              :detail="account.forecast_cents === null ? (account.forecast_error ? `AWS gave none: ${account.forecast_error}` : 'AWS gave none') : undefined"
            />
            <StatTile
              label="Monthly budget"
              :value="account.budget ? costMoney(account.budget.limit_cents, account.budget.currency) : 'None read'"
              :detail="budgetDescription(account)"
            />
          </dl>
        </template>
      </article>
    </section>

    <p v-if="costs.available && (costs.read_at || !costs.error)" class="cost-read text-xs text-ink-muted">
      <template v-if="costs.read_at">Read from AWS <RelativeTime :value="costs.read_at" :now="now" />. </template>
      Successful reads are kept for up to 6 hours; failed reads are retried after 10 minutes. Each Cost Explorer request costs $0.01.
      Cost Explorer's figures run up to a day behind<template v-if="costs.estimated"> and stay estimates until AWS closes the month</template>.
    </p>
  </div>
</template>

<script setup lang="ts">
// The configured accounts' AWS bill this month. Multiple accounts share
// combined totals and charts, with a separate budget for each account. Failed
// usage reads hide the combined totals but leave readable account details.
// A single account keeps its budget meter and three summary tiles.
import { computed } from 'vue';
import RelativeTime from './RelativeTime.vue';
import Sparkline from './charts/Sparkline.vue';
import StatTile from './charts/StatTile.vue';
import type { CostAccount, Costs } from '../types/costs';
import { budgetState, costMoney, monthName, periodLabel, serviceLabel, topServices } from '../utils/costView';
import { badgeClass, type Tone } from '../utils/tone';

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
  info: 'border-accent/40 bg-accent/5',
  muted: 'border-border bg-panel',
};
const FILL: Record<Tone, string> = {
  ok: 'bg-online',
  warn: 'bg-warn',
  bad: 'bg-danger',
  info: 'bg-accent',
  muted: 'bg-ink-muted',
};

function money(cents: number): string {
  return costMoney(cents, props.costs.currency);
}

const multipleAccounts = computed(() => props.costs.accounts.length > 1);
const scope = computed(() => props.costs.accounts.map((account) => account.label).join(' + '));
const accounts = computed(() => props.costs.accounts.map((account) => ({ ...account, state: account.error ? null : budgetState(account) })));
const state = computed(() => multipleAccounts.value ? null : budgetState(props.costs));
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

function budgetDescription(costs: Pick<CostAccount, 'budget' | 'budget_error'>): string {
  const b = costs.budget;
  if (!b) return costs.budget_error ? 'It could not be read' : 'No budget is named here';
  if (b.actual_cents !== null) return `AWS Budgets counts ${costMoney(b.actual_cents, b.currency)} spent`;
  return b.name;
}

const budgetDetail = computed(() => budgetDescription(props.costs));

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
