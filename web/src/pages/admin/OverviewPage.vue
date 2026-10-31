<template>
  <div class="space-y-8">
    <PageHeader title="Overview" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="refresh" />

    <Notice v-if="problems.length" tone="bad" role="alert" class="overview-problems">Could not read {{ problems.join('; ') }}.</Notice>

    <Notice v-if="showBilling && billing?.limits_hold" tone="bad" role="alert" class="limits-hold">
      Server limits are on hold: {{ holdSummary }}.
      <RouterLink to="/admin/billing" class="link">Review</RouterLink>
    </Notice>

    <dl class="overview-tiles grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-4">
      <StatTile
        label="Players online"
        to="/admin/players"
        :value="online ? formatCount(online.count) : '…'"
        :detail="online ? `${formatCount(playing)} playing, ${formatCount(online.count - playing)} in the lobby` : undefined"
      />
      <StatTile
        label="Servers live"
        to="/admin/servers"
        :value="fleet ? formatCount(fleet.shared.live + fleet.own.live) : '…'"
        :detail="fleet ? (fleet.enabled ? `${formatCount(playersHosted)} players on them` : 'Hosting is off') : undefined"
        :tone="fleet && !fleet.enabled ? 'warn' : undefined"
      >
        <template v-if="sparkValues.length" #trend>
          <Sparkline :values="sparkValues" label="Players on hosted servers, last 24 hours" :format="formatCount" />
        </template>
      </StatTile>
      <StatTile
        label="Machines"
        to="/admin/servers/machines"
        :value="fleet ? formatCount(machines.shared + machines.own) : '…'"
        :detail="fleet ? `${machines.shared} shared, ${machines.own} dedicated` : undefined"
      />
      <StatTile
        label="NovaWorld sign-in"
        to="/admin/system"
        :value="status ? (status.maintenance_enabled ? 'Maintenance' : 'Open') : '…'"
        :tone="status?.maintenance_enabled ? 'warn' : undefined"
      />
      <StatTile
        v-if="showBilling"
        label="Subscribers"
        to="/admin/billing"
        :value="billing ? (billing.enabled ? formatCount(subscribers) : 'Billing off') : '…'"
        :detail="billing?.enabled ? (pastDue ? `${pastDue} past due` : revenue ? `About ${revenue} a month` : undefined) : undefined"
        :tone="billing?.enabled && pastDue > 0 ? 'warn' : undefined"
      />
      <StatTile v-if="showBilling" label="AWS this month" to="/admin/billing/costs" :value="awsValue" :detail="awsDetail" :tone="budget && budget.tone !== 'ok' ? budget.tone : undefined" />
      <StatTile
        label="Jobs failed today"
        to="/admin/system/jobs"
        :value="jobs ? formatCount(jobs.attention_total) : '…'"
        :detail="jobs ? `${formatCount(jobs.totals.queued)} queued, ${formatCount(jobs.totals.running)} running` : undefined"
        :tone="jobs && jobs.attention_total > 0 ? 'warn' : undefined"
      />
      <StatTile
        label="Releases to approve"
        to="/admin/products"
        :value="attentionState.attention.value ? formatCount(awaiting) : '…'"
        :tone="awaiting > 0 ? 'warn' : undefined"
      />
    </dl>

    <AttentionList />
  </div>
</template>

<script setup lang="ts">
// /admin: the deployment at a glance, a tile a section, each opening the page
// that holds what it counts, then what needs an admin. It reads every 30
// seconds; each read stands alone, so one failing leaves the others shown.
// The money (subscribers, AWS, the limits hold) shows, and is read, only for
// the admin who sees billing (utils/billingAccess.ts).
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import AttentionList from '../../components/AttentionList.vue';
import Sparkline from '../../components/charts/Sparkline.vue';
import StatTile from '../../components/charts/StatTile.vue';
import Notice from '../../components/ui/Notice.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import { fetchHostedFleet, fetchServerStatus } from '../../api/admin';
import { fetchAdminBilling, limitsHoldSummary } from '../../api/adminBilling';
import { fetchTiers } from '../../api/billing';
import { fetchCosts } from '../../api/costs';
import { fetchFleetStats } from '../../api/hostedStats';
import { fetchJobsOverview } from '../../api/jobs';
import { fetchOnline } from '../../api/online';
import { useAttention } from '../../composables/useAttention';
import { usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import type { ServerStatus } from '../../types/admin';
import type { AdminBillingResponse, TiersResponse } from '../../types/billing';
import type { Costs } from '../../types/costs';
import type { FleetResponse } from '../../types/hostedServer';
import type { HostedStats } from '../../types/hostedStats';
import type { JobsOverview } from '../../types/jobs';
import type { OnlineResponse } from '../../types/online';
import { seesBilling } from '../../utils/billingAccess';
import { billingRevenue } from '../../utils/billingRevenue';
import { costMoney, worstCostBudget } from '../../utils/costView';
import { formatCount, formatMoney } from '../../utils/format';
import { playerValues } from '../../utils/statsView';

const POLL_MS = 30_000;

const attentionState = useAttention();
const { user } = useSession();
const showBilling = computed(() => seesBilling(user.value));

const fleet = ref<FleetResponse | null>(null);
const billing = ref<AdminBillingResponse | null>(null);
const tiers = ref<TiersResponse | null>(null);
const status = ref<ServerStatus | null>(null);
const online = ref<OnlineResponse | null>(null);
const stats = ref<HostedStats | null>(null);
const jobs = ref<JobsOverview | null>(null);
const costs = ref<Costs | null>(null);
const problems = ref<string[]>([]);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());

const playing = computed(() => (online.value?.players ?? []).filter((p) => p.state === 'playing').length);
const machines = computed(() => {
  const active = (fleet.value?.machines ?? []).filter((m) => m.state !== 'ended' && m.state !== 'failed');
  const shared = active.filter((m) => m.kind === 'shared').length;
  return { shared, own: active.length - shared };
});
const playersHosted = computed(() => (fleet.value?.servers ?? []).reduce((n, s) => n + (s.live ? s.players ?? 0 : 0), 0));
const sparkValues = computed(() => (stats.value ? playerValues(stats.value) : []));

const subscribers = computed(() => (billing.value?.subscriptions ?? []).filter((s) => s.item === 'tier' && s.entitled).length);
const pastDue = computed(() => (billing.value?.subscriptions ?? []).filter((s) => s.status === 'past_due').length);
const holdSummary = computed(() => (billing.value ? limitsHoldSummary(billing.value) : ''));
// What the subscriptions bring in a month at today's prices; null while
// billing is off or the prices are not read.
const revenue = computed(() => {
  if (!billing.value?.enabled || !tiers.value) return null;
  return formatMoney(billingRevenue(billing.value.subscriptions, tiers.value).total, tiers.value.currency);
});

const budget = computed(() => (costs.value ? worstCostBudget(costs.value) : null));
const awsValue = computed(() => {
  const c = costs.value;
  if (!c) return '…';
  if (!c.available) return 'Not read here';
  if (c.error) return 'Unknown';
  return costMoney(c.month_to_date_cents, c.currency);
});
const awsDetail = computed(() => {
  const c = costs.value;
  if (!c || !c.available || c.error) return undefined;
  if (budget.value && budget.value.tone !== 'ok') return `${budget.value.accountLabel ? `${budget.value.accountLabel}: ` : ''}${budget.value.detail}`;
  return c.forecast_cents === null ? undefined : `About ${costMoney(c.forecast_cents, c.currency)} by the month's end`;
});

const awaiting = computed(() => attentionState.attention.value?.findings.filter((f) => f.source === 'releases').length ?? 0);

const poll = usePolling({
  async read(signal) {
    const money = showBilling.value;
    const [f, b, s, o, st, j, co, t] = await Promise.all([
      fetchHostedFleet({ state: 'live' }, signal),
      money ? fetchAdminBilling(signal) : Promise.resolve(null),
      fetchServerStatus(signal),
      fetchOnline(signal),
      fetchFleetStats('24h', signal),
      fetchJobsOverview(signal),
      money ? fetchCosts(signal) : Promise.resolve(null),
      // The prices change rarely: read once.
      !money || tiers.value ? Promise.resolve(null) : fetchTiers(signal),
    ]);
    if (signal.aborted) return false;
    now.value = Date.now();
    const out: string[] = [];
    if (f.ok) fleet.value = f.value;
    else out.push(`the fleet (${f.error.message})`);
    if (b?.ok) billing.value = b.value;
    else if (b) out.push(`billing (${b.error.message})`);
    if (s.ok) status.value = s.value;
    else out.push(`NovaWorld's sign-in (${s.error.message})`);
    if (o.ok) online.value = o.value;
    else out.push(`who is online (${o.error.message})`);
    if (j.ok) jobs.value = j.value;
    else out.push(`the background jobs (${j.error.message})`);
    if (co?.ok) costs.value = co.value;
    else if (co) out.push(`AWS costs (${co.error.message})`);
    // The sparkline and the prices are niceties: their failure says nothing.
    if (st.ok) stats.value = st.value;
    if (t && t.ok) tiers.value = t.value;
    problems.value = out;
    loadedAt.value = now.value;
    return out.length === 0;
  },
  next: () => POLL_MS,
});

function refresh() {
  void poll.refresh();
  void attentionState.refresh();
}
</script>
