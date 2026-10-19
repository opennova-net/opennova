<template>
  <div class="space-y-8">
    <AdminPageHeader
      title="Overview"
      description="What needs you first, then every area at a glance."
      :crumbs="false"
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="refresh"
    />

    <p v-if="problems.length" role="alert" class="overview-problems rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      Not read: {{ problems.join(' ') }}
    </p>

    <div v-if="billing?.limits_hold" role="alert" class="limits-hold space-y-1 rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm">
      <p class="font-semibold text-danger">Hosted server limits are on hold</p>
      <p class="text-ink">
        A pass would stop or move {{ holdSummary }} for their hosting limits, so none is stopped or moved until a
        clean read from Stripe confirms them or an admin applies the stops.
        <RouterLink to="/admin/billing" class="text-accent hover:text-ink-bright">Review it on the Subscriptions page</RouterLink>.
      </p>
      <p v-if="billing.limits_hold_reason" class="font-mono text-xs text-ink-muted">{{ billing.limits_hold_reason }}</p>
    </div>

    <AttentionList />

    <section class="space-y-3" aria-labelledby="overview-areas">
      <h2 id="overview-areas" class="text-xl font-semibold text-ink">At a glance</h2>
      <div class="grid gap-4 xl:grid-cols-2">
        <OverviewArea :area="AREA.novaworld" :status="novaworldStatus">
          <OverviewFigure
            label="Sign-in"
            :value="status ? (status.maintenance_enabled ? 'Maintenance' : 'Open') : 'Unknown'"
            :tone="status?.maintenance_enabled ? 'warn' : undefined"
            :detail="status ? (status.maintenance_enabled ? 'Game clients cannot sign in' : 'Game clients can sign in') : 'Not read'"
          />
          <OverviewFigure
            label="Signed in now"
            :value="sessions ? formatCount(sessions.count) : 'Unknown'"
            :detail="sessions ? counted(hostingSessions, 'hosts a server', 'host a server') : 'Not read'"
          />
        </OverviewArea>

        <OverviewArea :area="AREA.hosting" :status="hostingStatus">
          <OverviewFigure
            label="Live servers"
            :value="fleet ? `${fleet.shared.live} of ${fleet.shared.room} shared` : 'Unknown'"
            :detail="fleet ? (fleet.enabled ? `${fleet.own.live} of ${fleet.own.room} own` : 'Hosting is off') : 'Not read'"
          />
          <OverviewFigure
            label="Machines"
            :value="fleet ? String(machines.shared + machines.own) : 'Unknown'"
            :detail="fleet ? `${machines.shared} shared, ${machines.own} own` : 'Not read'"
          />
          <OverviewFigure
            label="Players on hosted servers"
            :value="fleet ? formatCount(playersOnline) : 'Unknown'"
            :detail="sparkValues.length ? 'Now; the line is the last 24 hours' : 'Now, from the listings'"
          >
            <template v-if="sparkValues.length" #trend>
              <Sparkline :values="sparkValues" label="Players on hosted servers, last 24 hours" :format="formatCount" />
            </template>
          </OverviewFigure>
        </OverviewArea>

        <OverviewArea :area="AREA.people" :status="peopleStatus">
          <OverviewFigure
            label="Subscribers"
            :value="billingOn ? String(subscribers.total) : billing ? 'Billing off' : 'Unknown'"
            :detail="billingOn ? `Tier 1: ${subscribers.byTier[0]}, Tier 2: ${subscribers.byTier[1]}, Tier 3: ${subscribers.byTier[2]}` : billing ? 'Stripe is not configured here' : 'Not read'"
          />
          <OverviewFigure
            label="Payments past due"
            :value="billingOn ? String(pastDue) : billing ? 'Billing off' : 'Unknown'"
            :tone="billingOn && pastDue > 0 ? 'warn' : undefined"
            :detail="billingOn ? (pastDue === 0 ? 'Every payment went through' : 'Their servers keep running while Stripe retries') : undefined"
          />
          <OverviewFigure
            v-if="revenue !== null"
            label="Subscriptions a month"
            :value="revenue"
            approximate
            detail="At today's prices, before Stripe's fees and tax"
          />
          <OverviewFigure label="AWS this month" :value="awsValue" :tone="awsTone" :detail="awsDetail" />
        </OverviewArea>

        <OverviewArea :area="AREA.operations" :status="operationsStatus">
          <OverviewFigure label="Alarms firing" :value="alarmsValue" :tone="alarmsFiring > 0 ? 'bad' : undefined" :detail="alarmsDetail" />
          <OverviewFigure
            label="Background jobs"
            :value="jobs ? `${formatCount(jobs.totals.queued)} queued, ${formatCount(jobs.totals.running)} running` : 'Unknown'"
            :tone="jobs && jobs.attention_total > 0 ? 'warn' : undefined"
            :detail="jobs ? (jobs.attention_total === 0 ? 'None failed in the last day' : `${formatCount(jobs.attention_total)} failed in the last day`) : 'Not read'"
          />
        </OverviewArea>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/overview, the admin's home: what needs an admin first (AttentionList,
// the admin layout's read of GET /api/admin/attention), with a banner while the
// hosted servers' limits are on hold; then each area (utils/adminNav.ts) at a
// glance, its state as a badge in words and colour (the worst of what it holds,
// what needs you in it among them) and its key numbers, each from its own read
// so one failing leaves the rest (and the failure is named above):
//
//   NovaWorld         sign-in open or in maintenance (GET /api/admin/server-status),
//                     the sessions signed in and hosting (GET /api/admin/connections)
//   Hosting           live servers against the cap and the machines by kind
//                     (GET /api/admin/hosted), players now and the last 24 hours
//                     (GET /api/admin/hosted/stats)
//   People and money  subscribers by tier, payments past due, what the
//                     subscriptions bring in a month at today's prices (GET
//                     /api/admin/billing, /api/billing/tiers), and the month's
//                     AWS bill against the budget (GET /api/admin/costs)
//   Operations        the firing alarms (the attention read), the job queue
//                     and its failures (GET /api/admin/jobs/overview)
//
// Polled every 30 s; Refresh reads everything again, what needs you with it.
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import AttentionList from '../../components/AttentionList.vue';
import OverviewArea from '../../components/OverviewArea.vue';
import OverviewFigure from '../../components/OverviewFigure.vue';
import Sparkline from '../../components/charts/Sparkline.vue';
import { fetchConnections, fetchHostedFleet, fetchServerStatus } from '../../api/admin';
import { fetchAdminBilling, limitsHoldSummary } from '../../api/adminBilling';
import { fetchTiers } from '../../api/billing';
import { fetchCosts } from '../../api/costs';
import { fetchFleetStats } from '../../api/hostedStats';
import { fetchJobsOverview } from '../../api/jobs';
import { useAttention } from '../../composables/useAttention';
import { usePolling } from '../../composables/usePolling';
import type { ConnectionsResponse, ServerStatus } from '../../types/admin';
import type { AdminBillingResponse, TiersResponse } from '../../types/billing';
import type { Costs } from '../../types/costs';
import type { FleetResponse } from '../../types/hostedServer';
import type { HostedStats } from '../../types/hostedStats';
import type { JobsOverview } from '../../types/jobs';
import { ADMIN_AREAS, type AdminArea } from '../../utils/adminNav';
import { billingRevenue } from '../../utils/billingRevenue';
import { budgetState, costMoney } from '../../utils/costView';
import { formatCount, formatMoney } from '../../utils/format';
import type { Tone } from '../../utils/infraView';
import { areaAttention, counted, worstStatus, type AreaStatus } from '../../utils/overviewView';
import { playerValues } from '../../utils/statsView';

const POLL_MS = 30_000;

const AREA = Object.fromEntries(ADMIN_AREAS.map((a) => [a.id, a])) as Record<AdminArea['id'], AdminArea>;

const attentionState = useAttention();

const fleet = ref<FleetResponse | null>(null);
const billing = ref<AdminBillingResponse | null>(null);
const tiers = ref<TiersResponse | null>(null);
const status = ref<ServerStatus | null>(null);
const sessions = ref<ConnectionsResponse | null>(null);
const stats = ref<HostedStats | null>(null);
const jobs = ref<JobsOverview | null>(null);
const costs = ref<Costs | null>(null);
const problems = ref<string[]>([]);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());

// NovaWorld.
const hostingSessions = computed(() => (sessions.value?.connections ?? []).filter((c) => c.host !== null).length);
const novaworldStatus = computed<AreaStatus>(() =>
  worstStatus(
    [
      areaAttention(attentionState.attention.value, 'novaworld'),
      status.value?.maintenance_enabled ? { tone: 'warn', label: 'In maintenance' } : null,
    ],
    status.value ? { tone: 'ok', label: 'Open' } : { tone: 'idle', label: 'Not read' },
  ),
);

// Hosting.
const machines = computed(() => {
  const active = (fleet.value?.machines ?? []).filter((m) => m.state !== 'ended' && m.state !== 'failed');
  const shared = active.filter((m) => m.kind === 'shared').length;
  return { shared, own: active.length - shared };
});
const playersOnline = computed(() => (fleet.value?.servers ?? []).reduce((n, s) => n + (s.live ? s.players ?? 0 : 0), 0));
const sparkValues = computed(() => (stats.value ? playerValues(stats.value) : []));
const hostingStatus = computed<AreaStatus>(() => {
  let fallback: AreaStatus = { tone: 'idle', label: 'Not read' };
  if (fleet.value) fallback = fleet.value.enabled ? { tone: 'ok', label: 'Running' } : { tone: 'idle', label: 'Hosting is off' };
  return worstStatus([areaAttention(attentionState.attention.value, 'hosting')], fallback);
});

// People and money.
const billingOn = computed(() => billing.value?.enabled === true);
const subscribers = computed(() => {
  const tiered = (billing.value?.subscriptions ?? []).filter((s) => s.item === 'tier' && s.entitled);
  const byTier = [1, 2, 3].map((t) => tiered.filter((s) => s.tier === t).length);
  return { total: tiered.length, byTier };
});
const pastDue = computed(() => (billing.value?.subscriptions ?? []).filter((s) => s.status === 'past_due').length);
const holdSummary = computed(() => (billing.value ? limitsHoldSummary(billing.value) : ''));
// What the subscriptions bring in a month at today's prices; null while
// billing is off or the prices are not read.
const revenue = computed(() => {
  if (!billingOn.value || !tiers.value || !billing.value) return null;
  return formatMoney(billingRevenue(billing.value.subscriptions, tiers.value).total, tiers.value.currency);
});
const budget = computed(() => (costs.value?.available && !costs.value.error ? budgetState(costs.value) : null));
const awsTone = computed<Tone | undefined>(() => (budget.value && budget.value.tone !== 'ok' ? budget.value.tone : undefined));
const awsValue = computed(() => {
  const c = costs.value;
  if (!c) return 'Unknown';
  if (!c.available) return 'Not read here';
  if (c.error) return 'Unknown';
  return `${costMoney(c.month_to_date_cents, c.currency)} so far`;
});
const awsDetail = computed(() => {
  const c = costs.value;
  if (!c) return 'Not read';
  if (!c.available) return 'AWS costs are not read on this stack';
  if (c.error) return 'AWS could not be read';
  if (budget.value) return budget.value.detail;
  return c.forecast_cents === null ? 'No forecast yet' : `About ${costMoney(c.forecast_cents, c.currency)} by the month's end`;
});
const peopleStatus = computed<AreaStatus>(() => {
  const b = billing.value;
  return worstStatus(
    [
      b?.limits_hold ? { tone: 'bad', label: 'Server limits on hold' } : null,
      budget.value?.tone === 'bad' ? { tone: 'bad', label: 'Over the AWS budget' } : null,
      areaAttention(attentionState.attention.value, 'people'),
      billingOn.value && pastDue.value > 0 ? { tone: 'warn', label: counted(pastDue.value, 'payment past due', 'payments past due') } : null,
      budget.value?.tone === 'warn' ? { tone: 'warn', label: 'Near the AWS budget' } : null,
    ],
    b ? (b.enabled ? { tone: 'ok', label: 'Up to date' } : { tone: 'idle', label: 'Billing off' }) : { tone: 'idle', label: 'Not read' },
  );
});

// Operations.
const alarmsFiring = computed(() => attentionState.attention.value?.alarms.length ?? 0);
const alarmsValue = computed(() => {
  const a = attentionState.attention.value;
  if (!a) return 'Unknown';
  if (!a.alarms_read) return 'Not read';
  return String(a.alarms.length);
});
const alarmsDetail = computed(() => {
  const a = attentionState.attention.value;
  if (!a) return 'Not read';
  if (!a.alarms_read) return a.alarms_read_failed ? 'CloudWatch could not be read' : 'Not read on the local stack';
  return a.alarms.length === 0 ? 'Every CloudWatch alarm is quiet' : a.alarms.map((x) => x.name).join(', ');
});
const operationsStatus = computed<AreaStatus>(() =>
  worstStatus(
    [
      areaAttention(attentionState.attention.value, 'operations'),
      jobs.value && jobs.value.attention_total > 0 ? { tone: 'warn', label: counted(jobs.value.attention_total, 'job failed', 'jobs failed') } : null,
    ],
    jobs.value ? { tone: 'ok', label: 'Running' } : { tone: 'idle', label: 'Not read' },
  ),
);

const poll = usePolling({
  async read(signal) {
    const [f, b, s, c, st, j, co, t] = await Promise.all([
      fetchHostedFleet({ state: 'live' }, signal),
      fetchAdminBilling(signal),
      fetchServerStatus(signal),
      fetchConnections(signal),
      fetchFleetStats('24h', signal),
      fetchJobsOverview(signal),
      fetchCosts(signal),
      // The prices change rarely: read once.
      tiers.value ? Promise.resolve(null) : fetchTiers(signal),
    ]);
    if (signal.aborted) return false;
    now.value = Date.now();
    const out: string[] = [];
    if (f.ok) fleet.value = f.value;
    else out.push(`the fleet (${f.error.message})`);
    if (b.ok) billing.value = b.value;
    else out.push(`billing (${b.error.message})`);
    if (s.ok) status.value = s.value;
    else out.push(`NovaWorld's sign-in (${s.error.message})`);
    if (c.ok) sessions.value = c.value;
    else out.push(`NovaWorld's sessions (${c.error.message})`);
    if (j.ok) jobs.value = j.value;
    else out.push(`the background jobs (${j.error.message})`);
    if (co.ok) costs.value = co.value;
    else out.push(`AWS costs (${co.error.message})`);
    // The sparkline and the prices are niceties: their failure says nothing.
    if (st.ok) stats.value = st.value;
    if (t && t.ok) tiers.value = t.value;
    problems.value = out.length ? [`${out.join('; ')}.`] : [];
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
