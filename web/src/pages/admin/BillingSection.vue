<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Billing</h1>
        <p class="text-sm text-ink-muted">
          Support tier subscriptions as NovaWorld last read them from Stripe. Changes to a subscription happen in
          Stripe; the dashboard links open it there.
        </p>
      </div>
      <button type="button" :class="secondaryButton" :disabled="loading" @click="load">
        {{ loading ? 'Refreshing…' : 'Refresh' }}
      </button>
    </header>

    <div role="status" class="sr-only">{{ message }}</div>
    <div role="alert" class="sr-only">{{ error }}</div>
    <div v-if="message" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">{{ message }}</div>
    <div v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">{{ error }}</div>

    <div v-if="billing === null && loading" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading billing…
    </div>

    <p v-else-if="billing && !billing.enabled" class="rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted">
      Billing is off: Stripe is not configured on this NovaWorld service, so no support tier can be bought.
    </p>

    <template v-else-if="billing">
      <!-- The sync's health. -->
      <section class="space-y-4" aria-labelledby="billing-health">
        <h2 id="billing-health" class="sr-only">Sync with Stripe</h2>
        <dl class="grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-4">
          <StatTile label="Stripe mode" :value="billing.mode === 'live' ? 'Live' : 'Test'" :detail="billing.mode === 'live' ? 'Real payments' : 'Test cards only, no real payments'" />
          <StatTile
            label="Prices"
            :value="billing.prices_ok ? 'OK' : 'Wrong'"
            :detail="billing.prices_ok ? 'Every price is active, monthly and in the key\'s mode' : 'A price is missing, not monthly or in the other mode: checkouts are off until it is fixed'"
          />
          <StatTile
            label="Last full read from Stripe"
            :value="billing.last_reconcile_at ? formatRelative(billing.last_reconcile_at, now) : 'Never'"
            :detail="billing.last_reconcile_error ? `It failed: ${billing.last_reconcile_error}` : billing.last_reconcile_at ? formatUtc(billing.last_reconcile_at) : 'One runs at start and every 6 hours'"
          />
          <StatTile
            label="Customers waiting to sync"
            :value="String(billing.pending_events)"
            detail="Marked by a webhook event, read from Stripe next"
          />
        </dl>
        <div class="flex flex-wrap items-center gap-3">
          <button type="button" :disabled="busy" :class="primaryButton" @click="reconcile">Reconcile now</button>
          <p class="text-xs text-ink-muted">Reads every subscription from Stripe again, in the background.</p>
        </div>
      </section>

      <!-- Subscribers by tier, and the monthly total. -->
      <section class="space-y-3" aria-labelledby="billing-tiers">
        <h2 id="billing-tiers" class="text-xl font-semibold text-ink">Subscribers by tier</h2>
        <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
          <table class="min-w-full divide-y divide-border text-sm">
            <caption class="sr-only">Subscribers and monthly revenue by tier</caption>
            <thead>
              <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
                <th scope="col" class="px-4 py-2">Tier</th>
                <th scope="col" class="px-4 py-2 text-right">Subscribers</th>
                <th scope="col" class="px-4 py-2 text-right">Price</th>
                <th scope="col" class="px-4 py-2 text-right">Monthly</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-border text-ink">
              <tr v-for="row in revenue.rows" :key="row.label" class="billing-tier-row">
                <th scope="row" class="px-4 py-2 text-left font-medium">{{ row.label }}</th>
                <td class="px-4 py-2 text-right tabular-nums">{{ row.count }}</td>
                <td class="px-4 py-2 text-right tabular-nums">{{ row.price === null ? 'Not set' : money(row.price) }}</td>
                <td class="px-4 py-2 text-right tabular-nums">{{ row.price === null ? 'Unknown' : money(row.total) }}</td>
              </tr>
            </tbody>
            <tfoot>
              <tr class="bg-raised text-ink">
                <th scope="row" class="px-4 py-2 text-left font-semibold">Monthly total</th>
                <td class="px-4 py-2 text-right tabular-nums">{{ revenue.subscribers }}</td>
                <td></td>
                <td class="px-4 py-2 text-right font-semibold tabular-nums">{{ money(revenue.total) }}</td>
              </tr>
            </tfoot>
          </table>
        </div>
        <p class="text-xs text-ink-muted">
          Counted from the subscriptions that grant hosting now, at today's prices, before Stripe's fees and any tax:
          an estimate, not what Stripe pays out.
        </p>
      </section>

      <!-- Attention: past due, canceling, duplicates. -->
      <section class="grid gap-6 lg:grid-cols-2" aria-label="Subscriptions to watch">
        <div class="space-y-3">
          <h2 class="text-lg font-semibold text-ink">Payments past due ({{ pastDue.length }})</h2>
          <p v-if="pastDue.length === 0" class="text-sm text-ink-muted">None: every payment went through.</p>
          <ul v-else class="divide-y divide-border rounded-panel border border-border bg-panel text-sm">
            <li v-for="s in pastDue" :key="s.stripe_subscription_id" class="flex flex-wrap justify-between gap-2 px-4 py-2">
              <RouterLink :to="`/admin/users/${s.user_id}`" class="text-accent hover:text-ink-bright">{{ s.username || `Account ${s.user_id}` }}</RouterLink>
              <span class="text-ink-muted">{{ itemLabel(s) }}, Stripe retrying</span>
            </li>
          </ul>
        </div>
        <div class="space-y-3">
          <h2 class="text-lg font-semibold text-ink">Canceling ({{ canceling.length }})</h2>
          <p v-if="canceling.length === 0" class="text-sm text-ink-muted">None.</p>
          <ul v-else class="divide-y divide-border rounded-panel border border-border bg-panel text-sm">
            <li v-for="s in canceling" :key="s.stripe_subscription_id" class="flex flex-wrap justify-between gap-2 px-4 py-2">
              <RouterLink :to="`/admin/users/${s.user_id}`" class="text-accent hover:text-ink-bright">{{ s.username || `Account ${s.user_id}` }}</RouterLink>
              <span class="text-ink-muted">{{ itemLabel(s) }}, ends <RelativeTime :value="s.current_period_end" :now="now" /></span>
            </li>
          </ul>
        </div>
        <div v-if="billing.duplicates.length" class="space-y-3 lg:col-span-2">
          <h2 class="text-lg font-semibold text-warn">Accounts with two live tiers ({{ billing.duplicates.length }})</h2>
          <p class="text-sm text-ink-muted">Each pays twice; cancel one of their tiers in Stripe.</p>
          <ul class="flex flex-wrap gap-3 text-sm">
            <li v-for="userId in billing.duplicates" :key="userId">
              <RouterLink :to="`/admin/users/${userId}`" class="text-accent hover:text-ink-bright">{{ nameOf(userId) }}</RouterLink>
            </li>
          </ul>
        </div>
      </section>

      <!-- Orphans and the relink. -->
      <section class="space-y-3" aria-labelledby="billing-orphans">
        <h2 id="billing-orphans" class="text-xl font-semibold text-ink">Subscriptions no account owns ({{ billing.orphans.length }})</h2>
        <p class="text-sm text-ink-muted">
          Stripe subscriptions whose customer is not linked to an account here: another NovaWorld deployment's on the
          same Stripe account, or one whose link was lost (a database reset). Link a customer to the account it
          belongs to.
        </p>
        <p v-if="billing.orphans.length === 0" class="text-sm text-ink-muted">None.</p>
        <ul v-else class="space-y-3">
          <li v-for="o in billing.orphans" :key="`${o.stripe_customer_id}-${o.stripe_subscription_id}`" class="billing-orphan space-y-2 rounded-panel border border-border bg-panel p-4 text-sm">
            <p class="text-ink">
              Customer
              <a :href="stripeDashboardUrl('customers', o.stripe_customer_id, live)" target="_blank" rel="noopener noreferrer" class="font-mono text-accent hover:text-ink-bright">{{ o.stripe_customer_id }}</a>,
              {{ subscriptionStatusLabel(o.status).toLowerCase() }}<template v-if="o.username_hint">, named {{ o.username_hint }} in Stripe</template>
            </p>
            <form class="flex flex-wrap items-end gap-2" @submit.prevent="relink(o.stripe_customer_id)">
              <label class="space-y-1">
                <span class="block text-xs font-medium text-ink-muted">Account's username</span>
                <input
                  v-model="relinkNames[o.stripe_customer_id]"
                  :placeholder="o.username_hint"
                  autocomplete="off"
                  class="rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
                />
              </label>
              <button type="submit" :disabled="busy" :class="secondaryButton">Link<span class="sr-only"> customer {{ o.stripe_customer_id }}</span></button>
            </form>
          </li>
        </ul>
      </section>

      <!-- Every subscription. -->
      <section class="space-y-3" aria-labelledby="billing-subscriptions">
        <h2 id="billing-subscriptions" class="text-xl font-semibold text-ink">Subscriptions ({{ billing.subscriptions.length }})</h2>
        <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
          <table class="min-w-full divide-y divide-border text-sm">
            <caption class="sr-only">Every subscription</caption>
            <thead>
              <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
                <th scope="col" class="px-4 py-2">Account</th>
                <th scope="col" class="px-4 py-2">For</th>
                <th scope="col" class="px-4 py-2">Status</th>
                <th scope="col" class="px-4 py-2">Period ends</th>
                <th scope="col" class="px-4 py-2">In Stripe</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-border text-ink">
              <tr v-if="billing.subscriptions.length === 0">
                <td colspan="5" class="px-4 py-6 text-center text-ink-muted">No subscription yet.</td>
              </tr>
              <tr v-for="s in billing.subscriptions" :key="s.stripe_subscription_id">
                <th scope="row" class="px-4 py-2 text-left font-medium">
                  <RouterLink :to="`/admin/users/${s.user_id}`" class="text-accent hover:text-ink-bright">{{ s.username || `Account ${s.user_id}` }}</RouterLink>
                </th>
                <td class="whitespace-nowrap px-4 py-2">{{ itemLabel(s) }}</td>
                <td class="whitespace-nowrap px-4 py-2" :class="s.status === 'past_due' ? 'text-warn' : ''">
                  {{ subscriptionStatusLabel(s.status) }}<template v-if="s.cancel_at_period_end">, canceling</template>
                </td>
                <td class="whitespace-nowrap px-4 py-2"><RelativeTime :value="s.current_period_end" :now="now" /></td>
                <td class="px-4 py-2 text-xs">
                  <a :href="stripeDashboardUrl('customers', s.stripe_customer_id, live)" target="_blank" rel="noopener noreferrer" class="text-accent hover:text-ink-bright">Customer</a>,
                  <a :href="stripeDashboardUrl('subscriptions', s.stripe_subscription_id, live)" target="_blank" rel="noopener noreferrer" class="text-accent hover:text-ink-bright">subscription</a>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <!-- The webhook log. -->
      <section class="space-y-3" aria-labelledby="billing-events">
        <h2 id="billing-events" class="text-xl font-semibold text-ink">Recent webhook events</h2>
        <p v-if="billing.recent_events.length === 0" class="text-sm text-ink-muted">No event from Stripe yet.</p>
        <div v-else class="relative overflow-x-auto rounded-panel border border-border bg-panel">
          <table class="min-w-full divide-y divide-border text-sm">
            <caption class="sr-only">Recent webhook events, newest first</caption>
            <thead>
              <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
                <th scope="col" class="px-4 py-2">Received</th>
                <th scope="col" class="px-4 py-2">Event</th>
                <th scope="col" class="px-4 py-2">Outcome</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-border text-ink">
              <tr v-for="e in billing.recent_events" :key="e.event_id">
                <td class="whitespace-nowrap px-4 py-2"><RelativeTime :value="e.received_at" :now="now" /></td>
                <th scope="row" class="px-4 py-2 text-left font-normal">
                  <span class="block font-mono text-xs">{{ e.type }}</span>
                  <span class="block break-all font-mono text-xs text-ink-muted">{{ e.event_id }}</span>
                </th>
                <td class="px-4 py-2" :class="e.outcome.startsWith('fail') ? 'text-danger' : ''">{{ e.outcome || 'Waiting' }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/billing: the billing overview (GET /api/admin/billing): the Stripe
// mode, the prices' check, the last full read from Stripe with "Reconcile
// now" (POST /api/admin/billing/reconcile), subscribers by tier and the
// monthly total at the tiers' prices (GET /api/billing/tiers), the past-due,
// canceling and doubled accounts, the subscriptions no account owns with
// their relink (POST /api/admin/billing/link), every subscription, and the
// webhook event log.
import { computed, onMounted, reactive, ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import StatTile from '../../components/charts/StatTile.vue';
import { fetchAdminBilling, linkBillingCustomer, reconcileBilling, stripeDashboardUrl } from '../../api/adminBilling';
import { fetchTiers } from '../../api/billing';
import { listUsers } from '../../api/users';
import type { AdminBillingResponse, AdminSubscription, TiersResponse } from '../../types/billing';
import { subscriptionStatusLabel } from '../../utils/billingView';
import { billingRevenue } from '../../utils/billingRevenue';
import { formatMoney, formatRelative, formatUtc } from '../../utils/format';

const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-50';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const billing = ref<AdminBillingResponse | null>(null);
const tiers = ref<TiersResponse | null>(null);
const loading = ref(false);
const busy = ref(false);
const message = ref('');
const error = ref('');
const now = ref(Date.now());
const relinkNames = reactive<Record<string, string>>({});

const live = computed(() => billing.value?.mode === 'live');
const pastDue = computed(() => (billing.value?.subscriptions ?? []).filter((s) => s.status === 'past_due'));
const canceling = computed(() => (billing.value?.subscriptions ?? []).filter((s) => s.cancel_at_period_end && s.entitled));
const revenue = computed(() => billingRevenue(billing.value?.subscriptions ?? [], tiers.value));

function money(cents: number): string {
  return formatMoney(cents, tiers.value?.currency ?? 'usd');
}

function itemLabel(s: AdminSubscription): string {
  return s.item === 'own_machine' ? `Own machines: ${s.quantity}` : `Tier ${s.tier ?? '?'}`;
}

function nameOf(userId: number): string {
  const row = billing.value?.subscriptions.find((s) => s.user_id === userId);
  return row?.username || `Account ${userId}`;
}

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
}

async function load() {
  loading.value = true;
  const [b, t] = await Promise.all([fetchAdminBilling(), fetchTiers()]);
  loading.value = false;
  now.value = Date.now();
  if (!b.ok) {
    fail(b.error.message);
    return;
  }
  billing.value = b.value;
  if (t.ok) tiers.value = t.value;
}

async function reconcile() {
  if (busy.value) return;
  busy.value = true;
  const result = await reconcileBilling();
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  flash('Reconcile started: Stripe is read again in the background. Refresh in a minute to see it.');
}

async function relink(customerId: string) {
  if (busy.value) return;
  const name = (relinkNames[customerId] ?? '').trim();
  if (!name) {
    fail('Enter the username of the account this customer belongs to.');
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
    if (!window.confirm(`Link Stripe customer ${customerId} to ${user.username}? Its subscriptions then count for that account.`)) return;
    const result = await linkBillingCustomer(user.id, customerId);
    if (!result.ok) {
      fail(result.error.message);
      return;
    }
    flash(`Linked ${customerId} to ${user.username}.`);
    relinkNames[customerId] = '';
    await load();
  } finally {
    busy.value = false;
  }
}

onMounted(load);
</script>
