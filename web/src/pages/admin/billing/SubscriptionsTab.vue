<template>
  <div class="space-y-8">
    <PageHeader title="Billing" :tabs="BILLING_TABS" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template v-if="billing?.enabled" #meta>
        <Badge :tone="live ? 'ok' : 'info'">{{ live ? 'Stripe live' : 'Stripe test mode' }}</Badge>
        <Badge v-if="!billing.prices_ok" tone="bad" title="A price is missing, not monthly or in the other mode: checkouts are off">Prices wrong</Badge>
        <span v-if="billing.last_reconcile_at" :title="formatUtc(billing.last_reconcile_at)">Read from Stripe <RelativeTime :value="billing.last_reconcile_at" :now="now" /></span>
        <span v-else>Never read from Stripe</span>
        <Badge v-if="billing.last_reconcile_error" tone="bad" :title="billing.last_reconcile_error">Last read failed</Badge>
        <span v-if="billing.pending_events > 0">{{ countLabel(billing.pending_events, 'customer') }} waiting to sync</span>
      </template>
      <template v-if="billing?.enabled" #actions>
        <button type="button" class="btn-secondary" :disabled="busy" @click="reconcile">Reconcile now</button>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" />

    <div v-if="billing?.limits_hold" class="limits-hold notice notice-bad flex flex-wrap items-center justify-between gap-3" role="alert">
      <span :title="billing.limits_hold_reason ?? undefined">Server limits are on hold: a pass would stop or move {{ holdSummary }}.</span>
      <button type="button" class="btn-danger btn-sm" :disabled="busy" @click="applyStops">Apply the stops…</button>
    </div>
    <Notice v-if="billing?.unknown_prices.length" tone="warn" class="unknown-prices">
      Sold prices missing from the configuration: <span class="font-mono">{{ billing.unknown_prices.join(', ') }}</span>.
    </Notice>
    <Notice v-if="billing && !billing.enabled" class="billing-off">Billing is off: Stripe is not configured.</Notice>

    <div class="grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-5">
      <div class="billing-on-sale card flex flex-col gap-2 p-4">
        <span id="billing-on-sale-label" class="text-xs font-medium text-ink-muted">Subscriptions on sale</span>
        <button
          type="button"
          role="switch"
          class="flex items-center gap-3 text-left disabled:cursor-not-allowed disabled:opacity-50"
          aria-labelledby="billing-on-sale-label"
          :aria-checked="onSale ? 'true' : 'false'"
          :disabled="settings === null || savingSale"
          @click="toggleSale"
        >
          <span class="relative inline-flex h-6 w-11 shrink-0 rounded-full transition" :class="onSale ? 'bg-accent' : 'bg-hover'" aria-hidden="true">
            <span class="absolute top-0.5 h-5 w-5 rounded-full bg-ink-bright transition-all" :class="onSale ? 'left-[1.375rem]' : 'left-0.5'"></span>
          </span>
          <span class="text-2xl font-semibold text-ink">{{ settings === null ? '…' : onSale ? 'On' : 'Off' }}</span>
        </button>
      </div>
      <dl class="contents">
        <template v-if="billing?.enabled">
          <StatTile label="Subscribers" :value="formatCount(revenue.subscribers)" :detail="byTier" />
          <StatTile label="Past due" :value="formatCount(pastDue.length)" :tone="pastDue.length > 0 ? 'warn' : undefined" />
          <StatTile label="Revenue a month" :value="money(revenue.total)" :approximate="revenue.total > 0" />
        </template>
        <StatTile
          v-if="billing?.capacity"
          class="billing-capacity"
          label="Sold capacity"
          :value="`${billing.capacity.shared.sold} of ${billing.capacity.shared.sellable} shared`"
          :detail="capacityDetail"
          :tone="billing.capacity.oversold ? 'warn' : undefined"
        />
      </dl>
    </div>

    <template v-if="billing?.enabled">
      <section v-if="lookCount > 0" class="space-y-3" aria-labelledby="billing-look">
        <h2 id="billing-look" class="section-heading">Needs a look</h2>
        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">Account</th>
                <th scope="col">For</th>
                <th scope="col">Why</th>
                <th scope="col"><span class="sr-only">Actions</span></th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="s in pastDue" :key="`due-${s.stripe_subscription_id}`" class="billing-look-row">
                <th scope="row"><PlayerLink :id="s.user_id" :name="accountName(s)" /></th>
                <td class="whitespace-nowrap">{{ subscriptionItemLabel(s) }}</td>
                <td><Badge tone="warn">Past due</Badge></td>
                <td class="text-right"><a :href="stripeDashboardUrl('subscriptions', s.stripe_subscription_id, live)" target="_blank" rel="noopener noreferrer" class="btn-ghost btn-sm">In Stripe</a></td>
              </tr>
              <tr v-for="s in canceling" :key="`cancel-${s.stripe_subscription_id}`" class="billing-look-row">
                <th scope="row"><PlayerLink :id="s.user_id" :name="accountName(s)" /></th>
                <td class="whitespace-nowrap">{{ subscriptionItemLabel(s) }}</td>
                <td class="whitespace-nowrap"><Badge>Canceling</Badge> <span class="text-xs text-ink-muted">ends <RelativeTime :value="s.current_period_end" :now="now" /></span></td>
                <td class="text-right"><a :href="stripeDashboardUrl('subscriptions', s.stripe_subscription_id, live)" target="_blank" rel="noopener noreferrer" class="btn-ghost btn-sm">In Stripe</a></td>
              </tr>
              <tr v-for="userId in billing.duplicates" :key="`dup-${userId}`" class="billing-look-row">
                <th scope="row"><PlayerLink :id="userId" :name="nameOf(userId)" /></th>
                <td>Tier</td>
                <td><Badge tone="warn">Two live tiers</Badge></td>
                <td class="text-right">
                  <a v-if="customerOf(userId)" :href="stripeDashboardUrl('customers', customerOf(userId)!, live)" target="_blank" rel="noopener noreferrer" class="btn-ghost btn-sm">In Stripe</a>
                </td>
              </tr>
              <tr v-for="o in billing.orphans" :key="`orphan-${o.stripe_customer_id}-${o.stripe_subscription_id}`" class="billing-orphan">
                <th scope="row" class="font-normal">
                  <a :href="stripeDashboardUrl('customers', o.stripe_customer_id, live)" target="_blank" rel="noopener noreferrer" class="entity-link font-mono text-xs">{{ o.stripe_customer_id }}</a>
                  <span v-if="o.username_hint" class="block text-xs text-ink-muted">"{{ o.username_hint }}" in Stripe</span>
                </th>
                <td class="whitespace-nowrap">{{ subscriptionStatusLabel(o.status) }}</td>
                <td><Badge tone="bad">No account</Badge></td>
                <td>
                  <form class="flex justify-end gap-2" @submit.prevent="relink(o.stripe_customer_id)">
                    <label :for="`billing-relink-${o.stripe_customer_id}`" class="sr-only">Account's username for {{ o.stripe_customer_id }}</label>
                    <input
                      :id="`billing-relink-${o.stripe_customer_id}`"
                      v-model="relinkNames[o.stripe_customer_id]"
                      :placeholder="o.username_hint || 'Username'"
                      autocomplete="off"
                      class="field w-36 py-1"
                    />
                    <button type="submit" class="btn-secondary btn-sm" :disabled="busy">Link<span class="sr-only"> customer {{ o.stripe_customer_id }}</span></button>
                  </form>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section class="space-y-3" aria-labelledby="billing-tiers">
        <h2 id="billing-tiers" class="section-heading">Subscribers by tier</h2>
        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">Tier</th>
                <th scope="col" class="text-right">Subscribers</th>
                <th scope="col" class="text-right">Price</th>
                <th scope="col" class="text-right">A month</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="row in revenue.rows" :key="row.label" class="billing-tier-row">
                <th scope="row">{{ row.label }}</th>
                <td class="text-right tabular-nums">{{ row.count }}</td>
                <td class="text-right tabular-nums">{{ row.price === null ? 'Not set' : money(row.price) }}</td>
                <td class="text-right tabular-nums">{{ row.price === null ? 'Unknown' : money(row.total) }}</td>
              </tr>
            </tbody>
            <tfoot>
              <tr class="billing-total bg-raised">
                <th scope="row" class="text-left font-semibold">Total</th>
                <td class="text-right tabular-nums">{{ revenue.subscribers }}</td>
                <td></td>
                <td class="text-right font-semibold tabular-nums">{{ money(revenue.total) }}</td>
              </tr>
            </tfoot>
          </table>
        </div>
      </section>

      <section class="space-y-3" aria-labelledby="billing-subscriptions">
        <h2 id="billing-subscriptions" class="section-heading">All subscriptions</h2>
        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">Account</th>
                <th scope="col">For</th>
                <th scope="col">Status</th>
                <th scope="col">Period ends</th>
                <th scope="col">Stripe id</th>
              </tr>
            </thead>
            <tbody>
              <TableState v-if="billing.subscriptions.length === 0" :colspan="5" :rows="billing.subscriptions" empty="No subscriptions yet." />
              <tr v-for="s in billing.subscriptions" :key="s.stripe_subscription_id" class="billing-subscription">
                <th scope="row"><PlayerLink :id="s.user_id" :name="accountName(s)" /></th>
                <td class="whitespace-nowrap">{{ subscriptionItemLabel(s) }}</td>
                <td class="whitespace-nowrap">
                  <Badge :tone="subscriptionStatusTone(s.status)">{{ subscriptionStatusLabel(s.status) }}</Badge>
                  <Badge v-if="s.cancel_at_period_end && s.entitled" class="ml-1">Canceling</Badge>
                </td>
                <td class="whitespace-nowrap">
                  <RelativeTime v-if="s.current_period_end" :value="s.current_period_end" :now="now" />
                  <span v-else class="text-ink-muted">None</span>
                </td>
                <td>
                  <a :href="stripeDashboardUrl('subscriptions', s.stripe_subscription_id, live)" target="_blank" rel="noopener noreferrer" class="entity-link font-mono text-xs">{{ s.stripe_subscription_id }}</a>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>
    </template>

    <section v-if="billing" class="space-y-3" aria-labelledby="billing-grants">
      <h2 id="billing-grants" class="section-heading">Admin grants</h2>
      <div class="table-wrap">
        <table class="data-table">
          <thead>
            <tr>
              <th scope="col">Account</th>
              <th scope="col">Grant</th>
            </tr>
          </thead>
          <tbody>
            <TableState v-if="billing.comp_grants.length === 0" :colspan="2" :rows="billing.comp_grants" empty="No grants." />
            <tr v-for="g in billing.comp_grants" :key="g.user_id" class="billing-grant">
              <th scope="row"><PlayerLink :id="g.user_id" :name="g.username || `Account ${g.user_id}`" /></th>
              <td>{{ grantText(g) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <details v-if="billing?.enabled" class="billing-events space-y-3">
      <summary class="section-heading">Recent webhook events</summary>
      <div class="table-wrap mt-3">
        <table class="data-table">
          <thead>
            <tr>
              <th scope="col">Received</th>
              <th scope="col">Event</th>
              <th scope="col">Outcome</th>
            </tr>
          </thead>
          <tbody>
            <TableState v-if="billing.recent_events.length === 0" :colspan="3" :rows="billing.recent_events" empty="No events yet." />
            <tr v-for="e in billing.recent_events" :key="e.event_id">
              <td class="whitespace-nowrap"><RelativeTime :value="e.received_at" :now="now" /></td>
              <th scope="row" class="font-normal">
                <span class="block font-mono text-xs">{{ e.type }}</span>
                <span class="block break-all font-mono text-xs text-ink-muted">{{ e.event_id }}</span>
              </th>
              <td :class="e.outcome.startsWith('fail') ? 'text-danger' : ''">{{ e.outcome || 'Waiting' }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </details>
  </div>
</template>

<script setup lang="ts">
// /admin/billing: subscriptions as last read from Stripe, whether they sell
// (the admin's on/off), the limits hold, what needs a look (past due,
// canceling, two live tiers, subscriptions no account owns with their relink),
// subscribers by tier, every subscription, the admin grants and the webhook
// log. Read on open and on Refresh.
import { computed, reactive, ref } from 'vue';
import RelativeTime from '../../../components/RelativeTime.vue';
import StatTile from '../../../components/charts/StatTile.vue';
import PlayerLink from '../../../components/links/PlayerLink.vue';
import Badge from '../../../components/ui/Badge.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import Notice from '../../../components/ui/Notice.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TableState from '../../../components/ui/TableState.vue';
import {
  applyLimitStops,
  fetchAdminBilling,
  limitsHoldSummary,
  linkBillingCustomer,
  reconcileBilling,
  stripeDashboardUrl,
} from '../../../api/adminBilling';
import { fetchTiers } from '../../../api/billing';
import { fetchSettings, saveSettings } from '../../../api/settings';
import { listUsers } from '../../../api/users';
import { confirmAction } from '../../../composables/useConfirm';
import { usePolling } from '../../../composables/usePolling';
import type { AdminBillingResponse, AdminCompGrant, BillingSubscriptionRow, TiersResponse } from '../../../types/billing';
import type { ServiceSettings } from '../../../types/settings';
import { BILLING_TABS } from '../../../utils/adminNav';
import { billingRevenue } from '../../../utils/billingRevenue';
import { subscriptionItemLabel, subscriptionStatusLabel, subscriptionStatusTone } from '../../../utils/billingView';
import { countLabel, formatCount, formatMoney, formatUtc } from '../../../utils/format';

const billing = ref<AdminBillingResponse | null>(null);
const tiers = ref<TiersResponse | null>(null);
const settings = ref<ServiceSettings | null>(null);
const busy = ref(false);
const savingSale = ref(false);
const message = ref('');
const error = ref('');
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const relinkNames = reactive<Record<string, string>>({});

const live = computed(() => billing.value?.mode === 'live');
const onSale = computed(() => settings.value?.subscriptions_enabled === true);
const pastDue = computed(() => (billing.value?.subscriptions ?? []).filter((s) => s.status === 'past_due'));
const canceling = computed(() => (billing.value?.subscriptions ?? []).filter((s) => s.cancel_at_period_end && s.entitled));
const revenue = computed(() => billingRevenue(billing.value?.subscriptions ?? [], tiers.value));
const holdSummary = computed(() => (billing.value ? limitsHoldSummary(billing.value) : ''));
const lookCount = computed(
  () => pastDue.value.length + canceling.value.length + (billing.value?.duplicates.length ?? 0) + (billing.value?.orphans.length ?? 0),
);

// "2 on tier 1, 1 on tier 3".
const byTier = computed(() => {
  const parts = revenue.value.rows.slice(0, 3).filter((r) => r.count > 0).map((r) => `${r.count} on ${r.label.toLowerCase()}`);
  return parts.length ? parts.join(', ') : undefined;
});

const capacityDetail = computed(() => {
  const c = billing.value?.capacity;
  if (!c) return undefined;
  const rest = `${c.own_machines.sold} of ${c.own_machines.sellable} dedicated, ${c.addresses.sold} of ${c.addresses.sellable} addresses`;
  return c.oversold ? `Oversold. ${rest}` : rest;
});

function money(cents: number): string {
  return formatMoney(cents, tiers.value?.currency ?? 'usd');
}

function accountName(s: BillingSubscriptionRow): string {
  return s.username || `Account ${s.user_id}`;
}

function nameOf(userId: number): string {
  const row = billing.value?.subscriptions.find((s) => s.user_id === userId);
  return row ? accountName(row) : `Account ${userId}`;
}

function customerOf(userId: number): string | null {
  return billing.value?.subscriptions.find((s) => s.user_id === userId)?.stripe_customer_id || null;
}

// "3 servers on shared machines, 1 dedicated machine and 1 static address".
function grantText(g: AdminCompGrant): string {
  const parts = [
    ...(g.servers > 0 ? [countLabel(g.servers, 'server on shared machines', 'servers on shared machines')] : []),
    ...(g.own_machines > 0 ? [countLabel(g.own_machines, 'dedicated machine')] : []),
    ...(g.addresses > 0 ? [countLabel(g.addresses, 'static address', 'static addresses')] : []),
  ];
  return parts.length > 1 ? `${parts.slice(0, -1).join(', ')} and ${parts[parts.length - 1]}` : (parts[0] ?? '');
}

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
}

const poll = usePolling({
  async read(signal) {
    const [b, t, s] = await Promise.all([fetchAdminBilling(signal), fetchTiers(signal), fetchSettings(signal)]);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (t.ok) tiers.value = t.value;
    if (s.ok) settings.value = s.value;
    if (b.ok) billing.value = b.value;
    const failure = !b.ok ? b.error : !s.ok ? s.error : null;
    if (failure) {
      fail(failure.message);
      return false;
    }
    loadedAt.value = now.value;
    error.value = '';
    return true;
  },
  next: () => null,
});

// Subscriptions on or off sale, saved at once (the settings are one PUT: the
// fleet's as read just now go with it).
async function toggleSale() {
  const known = settings.value;
  if (!known || savingSale.value) return;
  savingSale.value = true;
  const fresh = await fetchSettings();
  const base = fresh.ok ? fresh.value : known;
  const result = await saveSettings({ ...base.fleet, subscriptions_enabled: !known.subscriptions_enabled });
  savingSale.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  settings.value = result.value;
  flash(result.value.subscriptions_enabled ? 'Subscriptions are on sale.' : 'Subscriptions are off sale.');
}

async function reconcile() {
  if (busy.value) return;
  busy.value = true;
  const result = await reconcileBilling();
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash('Reconcile started.');
}

async function applyStops() {
  if (busy.value) return;
  if (!await confirmAction(`Stop or move ${holdSummary.value} now? Their matches end for everyone on them.`)) return;
  busy.value = true;
  const result = await applyLimitStops();
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash('The stops apply on the next pass.');
}

async function relink(customerId: string) {
  if (busy.value) return;
  const name = (relinkNames[customerId] ?? '').trim();
  if (!name) {
    fail('Enter the account\'s username.');
    return;
  }
  busy.value = true;
  try {
    const users = await listUsers();
    if (!users.ok) {
      fail(users.error.message);
      return;
    }
    const user = users.value.find((u) => u.username.toLowerCase() === name.toLowerCase());
    if (!user) {
      fail(`No account is named ${name}.`);
      return;
    }
    if (!await confirmAction(`Link ${customerId} to ${user.username}? Its subscriptions count for that account.`)) return;
    const result = await linkBillingCustomer(user.id, customerId);
    if (!result.ok) {
      fail(result.error.message);
      return;
    }
    flash(`Linked ${customerId} to ${user.username}.`);
    relinkNames[customerId] = '';
    await poll.refresh();
  } finally {
    busy.value = false;
  }
}
</script>
