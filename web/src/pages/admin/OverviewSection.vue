<template>
  <div class="space-y-8">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Overview</h1>
        <p class="text-sm text-ink-muted">NovaWorld at a glance: hosting, the fleet, players, subscribers and the service.</p>
      </div>
      <div class="flex items-center gap-3">
        <p v-if="loadedAt" class="text-xs text-ink-muted">Updated <RelativeTime :value="loadedAt" :now="now" /></p>
        <button type="button" :class="secondaryButton" :disabled="poll.loading.value" @click="poll.refresh">
          {{ poll.loading.value ? 'Refreshing…' : 'Refresh' }}
        </button>
      </div>
    </header>

    <p v-if="problems.length" role="alert" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      {{ problems.join(' ') }}
    </p>

    <div v-if="billing?.limits_hold" role="alert" class="limits-hold space-y-1 rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm">
      <p class="font-semibold text-danger">Hosted server limits are on hold</p>
      <p class="text-ink">
        A pass would stop {{ holdSummary }} for their hosting limits, so none is stopped until a clean read from Stripe
        confirms them or an admin applies the stops.
        <RouterLink to="/admin/billing" class="text-accent hover:text-ink-bright">Review it on the Billing page</RouterLink>.
      </p>
      <p v-if="billing.limits_hold_reason" class="font-mono text-xs text-ink-muted">{{ billing.limits_hold_reason }}</p>
    </div>

    <AttentionList />

    <dl class="grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-3">
      <StatTile
        label="Live servers"
        :value="fleet ? `${fleet.shared.live} of ${fleet.shared.room} shared` : 'Unknown'"
        :detail="fleet ? (fleet.enabled ? `${fleet.own.live} of ${fleet.own.room} own` : 'Hosting is off') : 'The fleet could not be read'"
      />
      <StatTile
        label="Machines"
        :value="fleet ? String(machines.shared + machines.own) : 'Unknown'"
        :detail="fleet ? `${machines.shared} shared, ${machines.own} own` : undefined"
      />
      <StatTile
        label="Players on hosted servers"
        :value="fleet ? formatCount(playersOnline) : 'Unknown'"
        :detail="sparkValues.length ? 'Right now; the line is the last 24 hours, all servers together' : 'Right now, from the listings'"
      >
        <template v-if="sparkValues.length" #trend>
          <Sparkline :values="sparkValues" label="Players on hosted servers, last 24 hours" :format="formatCount" />
        </template>
      </StatTile>
      <StatTile
        label="Subscribers"
        :value="billingOn ? String(subscribers.total) : billing ? 'Off' : 'Unknown'"
        :detail="billingOn ? `Tier 1: ${subscribers.byTier[0]}, Tier 2: ${subscribers.byTier[1]}, Tier 3: ${subscribers.byTier[2]}` : billingNote"
      />
      <StatTile
        label="Payments past due"
        :value="billingOn ? String(pastDue) : billing ? 'Off' : 'Unknown'"
        :detail="billingOn ? (pastDue === 0 ? 'Every payment went through' : 'Their servers keep running while Stripe retries') : billingNote"
      />
      <StatTile
        label="NovaWorld service"
        :value="status ? (status.maintenance_enabled ? 'Maintenance' : 'Open') : 'Unknown'"
        :detail="status ? (status.maintenance_enabled ? 'Game clients cannot sign in' : 'Game clients can sign in') : undefined"
      />
    </dl>

    <nav aria-label="Admin sections" class="flex flex-wrap gap-4 text-sm">
      <RouterLink to="/admin/fleet" class="text-accent hover:text-ink-bright">The fleet</RouterLink>
      <RouterLink to="/admin/billing" class="text-accent hover:text-ink-bright">Billing</RouterLink>
      <RouterLink to="/admin/novaworld" class="text-accent hover:text-ink-bright">NovaWorld's status and sessions</RouterLink>
      <RouterLink to="/admin/jobs" class="text-accent hover:text-ink-bright">Background jobs</RouterLink>
    </nav>
  </div>
</template>

<script setup lang="ts">
// /admin/overview: what needs an admin first (AttentionList, the admin
// layout's read of GET /api/admin/attention), then the headline numbers, each
// from its own read so one failing leaves the rest: live servers against the
// cap and the machines by
// kind (GET /api/admin/hosted), players on hosted servers now with the fleet's
// last 24 hours (GET /api/admin/hosted/stats), subscribers by tier and
// payments past due with a banner while the hosted servers' limits are on
// hold (GET /api/admin/billing), and the NovaWorld service's maintenance mode
// (GET /api/admin/server-status). Polled every 30 s.
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import AttentionList from '../../components/AttentionList.vue';
import RelativeTime from '../../components/RelativeTime.vue';
import Sparkline from '../../components/charts/Sparkline.vue';
import StatTile from '../../components/charts/StatTile.vue';
import { fetchHostedFleet, fetchServerStatus } from '../../api/admin';
import { fetchAdminBilling, limitsHoldSummary } from '../../api/adminBilling';
import { fetchFleetStats } from '../../api/hostedStats';
import { usePolling } from '../../composables/usePolling';
import type { ServerStatus } from '../../types/admin';
import type { AdminBillingResponse } from '../../types/billing';
import type { FleetResponse } from '../../types/hostedServer';
import type { HostedStats } from '../../types/hostedStats';
import { formatCount } from '../../utils/format';
import { playerValues } from '../../utils/statsView';

const POLL_MS = 30_000;

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const fleet = ref<FleetResponse | null>(null);
const billing = ref<AdminBillingResponse | null>(null);
const status = ref<ServerStatus | null>(null);
const stats = ref<HostedStats | null>(null);
const problems = ref<string[]>([]);
const billingNote = ref<string | undefined>(undefined);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());

const machines = computed(() => {
  const active = (fleet.value?.machines ?? []).filter((m) => m.state !== 'ended' && m.state !== 'failed');
  const shared = active.filter((m) => m.kind === 'shared').length;
  return { shared, own: active.length - shared };
});

const playersOnline = computed(() => (fleet.value?.servers ?? []).reduce((n, s) => n + (s.live ? s.players ?? 0 : 0), 0));
const sparkValues = computed(() => (stats.value ? playerValues(stats.value) : []));

const subscribers = computed(() => {
  const tiers = (billing.value?.subscriptions ?? []).filter((s) => s.item === 'tier' && s.entitled);
  const byTier = [1, 2, 3].map((t) => tiers.filter((s) => s.tier === t).length);
  return { total: tiers.length, byTier };
});
const pastDue = computed(() => (billing.value?.subscriptions ?? []).filter((s) => s.status === 'past_due').length);
const holdSummary = computed(() => (billing.value ? limitsHoldSummary(billing.value) : ''));
const billingOn = computed(() => billing.value?.enabled === true);

const poll = usePolling({
  async read(signal) {
    const [f, b, s, st] = await Promise.all([
      fetchHostedFleet({ state: 'live' }, signal),
      fetchAdminBilling(signal),
      fetchServerStatus(signal),
      fetchFleetStats('24h', signal),
    ]);
    if (signal.aborted) return false;
    now.value = Date.now();
    const out: string[] = [];
    if (f.ok) fleet.value = f.value;
    else out.push(`The fleet: ${f.error.message}`);
    if (b.ok) {
      billing.value = b.value;
      billingNote.value = b.value.enabled ? undefined : 'Billing is off';
    } else {
      out.push(`Billing: ${b.error.message}`);
    }
    if (s.ok) status.value = s.value;
    else out.push(`The service: ${s.error.message}`);
    // The sparkline is a nicety: its failure says nothing.
    if (st.ok) stats.value = st.value;
    problems.value = out;
    loadedAt.value = now.value;
    return out.length === 0;
  },
  next: () => POLL_MS,
});
</script>
