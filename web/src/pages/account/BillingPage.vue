<template>
  <div class="space-y-8">
    <PageHeader title="Billing">
      <template v-if="billing?.enabled && billing.can_manage && tiers?.enabled" #actions>
        <button type="button" class="btn-secondary" :disabled="busy" @click="portal()">Manage in Stripe</button>
      </template>
    </PageHeader>

    <LiveNotices :message="statusText" :error="alertText" />

    <p v-if="loading" class="text-sm text-ink-muted">Loading…</p>

    <Notice v-else-if="loadError" tone="bad">
      {{ loadError }} <button type="button" class="btn-ghost btn-sm" @click="retry">Try again</button>
    </Notice>

    <p v-else-if="!tiers?.enabled || !billing?.enabled" class="billing-closed text-sm text-ink-muted">Billing is not open yet.</p>

    <template v-else-if="tiers && billing">
      <!-- Past due (the tier's payment, a dedicated machine's or an address's):
           the servers keep running while Stripe retries. -->
      <Notice v-if="pastDue" id="billing-past-due" tone="warn" class="flex flex-wrap items-center justify-between gap-3">
        <span>A payment did not go through; new servers can't start until it does.</span>
        <button type="button" class="btn-warn btn-sm" :disabled="busy" @click="portal()">Update payment method</button>
      </Notice>

      <section class="space-y-3" aria-labelledby="billing-tier-heading">
        <h2 id="billing-tier-heading" class="section-heading">Your tier</h2>
        <dl id="billing-tier" class="card grid gap-x-6 gap-y-2 p-4 text-sm sm:grid-cols-[auto_1fr]">
          <dt class="text-ink-muted">Tier</dt>
          <dd class="text-ink">
            <template v-if="liveTier && billing.subscription">Tier {{ billing.subscription.tier }}, up to {{ plural(tierServers, 'server') }}</template>
            <template v-else>None</template>
          </dd>
          <template v-if="liveTier && billing.subscription">
            <dt class="text-ink-muted">Status</dt>
            <dd class="text-ink">{{ subscriptionStatusLabel(billing.subscription.status) }}</dd>
            <dt class="text-ink-muted">{{ billing.subscription.cancel_at_period_end ? 'Ends' : 'Renews' }}</dt>
            <dd class="text-ink">
              <time :datetime="isoTime(billing.subscription.current_period_end)" :title="formatUtc(billing.subscription.current_period_end)">
                {{ formatDate(billing.subscription.current_period_end) }}
              </time>
            </dd>
          </template>
          <template v-if="billing.granted">
            <dt class="text-ink-muted">Granted</dt>
            <dd id="billing-granted" class="text-ink">{{ grantText }}</dd>
          </template>
          <template v-if="billing.shared_servers > 0">
            <dt class="text-ink-muted">Live on shared machines</dt>
            <dd class="text-ink">{{ billing.shared_used }} of {{ billing.shared_servers }}</dd>
          </template>
        </dl>
        <p v-if="liveTier && billing.subscription?.cancel_at_period_end" id="billing-canceled" class="text-sm text-warn">
          Canceled: when it ends, {{ afterTier }}.
        </p>
      </section>

      <!-- The admin's Subscriptions Enabled off: nothing sells; what is paid for is managed above. -->
      <p v-if="!tiers.on_sale" id="billing-off-sale" class="text-sm text-ink-muted">Nothing is on sale right now.</p>

      <section v-if="tiers.on_sale" class="space-y-4" aria-labelledby="billing-tiers">
        <h2 id="billing-tiers" class="section-heading">{{ liveTier ? 'Change tier' : 'Pick a tier' }}</h2>
        <p v-if="!liveTier && !billing.can_subscribe" class="text-sm text-warn">Your account cannot subscribe right now.</p>
        <TierPicker
          :tiers="tiers.tiers"
          :currency="tiers.currency"
          :mode="liveTier ? 'manage' : 'subscribe'"
          :current-tier="billing.subscription?.tier ?? null"
          :used="billing.shared_used - billing.shared_stopping"
          :busy="busy || waiting"
          :disabled="liveTier ? !billing.can_manage : !billing.can_subscribe"
          :selected-tier="liveTier ? null : selectedTier"
          :selection-only="!liveTier"
          base-id="billing"
          @choose="onChoose"
        />
        <div v-if="!liveTier && selectedOffer" id="billing-review" class="card flex flex-wrap items-center justify-between gap-3 border-accent/40 p-4">
          <div class="space-y-1">
            <h3 id="billing-review-heading" ref="reviewHeading" tabindex="-1" class="font-semibold text-ink-bright focus:outline-none">
              Tier {{ selectedOffer.tier }}<template v-if="selectedOffer.amount !== null">: {{ formatMoney(selectedOffer.amount, tiers.currency) }} a month</template>
            </h3>
            <p v-if="selectedOffer.amount === null" class="text-sm text-warn">No price yet.</p>
            <p v-else-if="!selectedOffer.available" class="text-sm text-warn">{{ HOSTING_FULL }}</p>
          </div>
          <button
            type="button"
            class="btn-primary"
            :disabled="busy || waiting || !billing.can_subscribe || selectedOffer.amount === null || !selectedOffer.available"
            @click="checkout(tierItem(selectedOffer.tier))"
          >
            Continue to checkout
          </button>
        </div>
        <p class="billing-fine-print text-xs text-ink-muted">
          Monthly in {{ tiers.currency.toUpperCase() }} through Stripe until you cancel; see the
          <RouterLink to="/terms" class="link">terms</RouterLink> and <RouterLink to="/refunds" class="link">refund policy</RouterLink>.
        </p>
      </section>

      <!-- Dedicated machines: subscriptions of their own, no tier needed. -->
      <section v-if="tiers.own_machine && (tiers.on_sale || billing.own_machine_subscriptions.length > 0)" class="space-y-3" aria-labelledby="billing-own-machine">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <h2 id="billing-own-machine" class="section-heading">Dedicated machines</h2>
          <div class="flex flex-wrap items-center gap-3">
            <span v-if="tiers.on_sale" class="text-sm text-ink-muted">
              {{ tiers.own_machine.amount === null ? 'No price yet' : `${formatMoney(tiers.own_machine.amount, tiers.currency)} a month each` }}
            </span>
            <button
              v-if="tiers.on_sale"
              type="button"
              :class="billing.own_machine_subscriptions.length > 0 ? 'btn-secondary btn-sm' : 'btn-primary btn-sm'"
              :disabled="busy || waiting || ownMachineBlock !== ''"
              :aria-describedby="ownMachineBlock ? 'billing-own-machine-block' : undefined"
              @click="checkout('own_machine')"
            >
              {{ billing.own_machine_subscriptions.length > 0 ? 'Add another' : 'Add a dedicated machine' }}
            </button>
            <button v-if="billing.own_machine_subscriptions.length > 0" type="button" class="btn-secondary btn-sm" :disabled="busy || !billing.can_manage" @click="portal()">
              Cancel in Stripe
            </button>
          </div>
        </div>
        <p v-if="tiers.on_sale && ownMachineBlock" id="billing-own-machine-block" class="text-xs text-ink-muted">{{ ownMachineBlock }}</p>
        <div v-if="billing.own_machine_subscriptions.length > 0" class="table-wrap">
          <table id="billing-own-machine-subscriptions" class="data-table">
            <thead>
              <tr>
                <th scope="col">Machines</th>
                <th scope="col">Status</th>
                <th scope="col">Renews</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="s in billing.own_machine_subscriptions" :key="s.stripe_subscription_id">
                <td>{{ s.quantity }}</td>
                <td>{{ subscriptionStatusLabel(s.status) }}</td>
                <td>
                  <template v-if="s.cancel_at_period_end">Ends </template>
                  <time :datetime="isoTime(s.current_period_end)" :title="formatUtc(s.current_period_end)">{{ formatDate(s.current_period_end) }}</time>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>
    </template>

    <StaticAddresses
      v-if="!loading && !loadError"
      :billing="billing"
      :offer="tiers?.on_sale && billing?.enabled ? (tiers.static_address ?? null) : null"
      :currency="tiers?.currency"
      :add-block="addressBlock"
      :busy="busy || waiting"
      @add="checkout('static_address')"
      @portal="(id) => portal(id)"
      @notice="(text) => (notice = text)"
      @error="(text) => (alertText = text)"
      @session-ended="onSessionEnded"
    />
  </div>
</template>

<script setup lang="ts">
// /me/billing: the account's tier (GET /api/me/billing) and the tiers on
// offer (GET /api/billing/tiers), its dedicated machines and its static
// addresses (StaticAddresses). With no tier, a chosen tier is reviewed, then
// Continue to checkout starts a Stripe Checkout (POST /api/billing/checkout);
// with one, every change, the payment method and a cancellation open Stripe's
// billing portal (POST /api/billing/portal), as does a checkout for a tier
// the account already has. Back from a checkout (?checkout=success) the page
// asks the service to read Stripe now (POST /api/me/billing/sync), then reads
// the account every 2 s backing off to 5 s for a minute until the payment is
// confirmed, then every 30 s, and drops the query once confirmed; a sync that
// finds no billing account drops it with nothing claimed, and a failed read
// holds the confirmation back until "Try again". ?checkout=cancel expires the
// open session (POST /api/billing/checkout/cancel).
import { computed, nextTick, onMounted, onUnmounted, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import StaticAddresses from '../../components/StaticAddresses.vue';
import TierPicker from '../../components/TierPicker.vue';
import LiveNotices from '../../components/ui/LiveNotices.vue';
import Notice from '../../components/ui/Notice.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import {
  cancelCheckout,
  fetchMyBilling,
  fetchTiers,
  openPortal,
  startCheckout,
  syncMyBilling,
  tierItem,
  type BillingError,
} from '../../api/billing';
import { useSession } from '../../composables/useSession';
import type { BillingItem, MyBilling, TierNumber, TiersResponse } from '../../types/billing';
import {
  activeAddressSubscriptions,
  checkoutConfirmed,
  hasLiveTier,
  HOSTING_FULL,
  recallCheckout,
  rememberCheckout,
  type PendingCheckout,
  subscriptionStatusLabel,
} from '../../utils/billingView';
import { formatDate, formatMoney, formatUtc, isoTime } from '../../utils/format';
import { navigateToStripe } from '../../utils/navigate';

// The wait for Stripe's confirmation after a checkout: from 2 s backing off
// to 5 s for a minute, then every 30 s.
const CONFIRM_FIRST_MS = 2_000;
const CONFIRM_MAX_MS = 5_000;
const CONFIRM_WINDOW_MS = 60_000;
const CONFIRM_SLOW_MS = 30_000;

const CONFIRMING_TEXT = 'Confirming your payment with Stripe…';
const PENDING_TEXT = 'Your payment went through; waiting for Stripe to confirm it.';
const BAD_STRIPE_URL = "Stripe's address in the reply looked wrong, so this page did not follow it. Try again.";

const route = useRoute();
const router = useRouter();
const { refresh: refreshSession } = useSession();

const tiers = ref<TiersResponse | null>(null);
const billing = ref<MyBilling | null>(null);
const loading = ref(true);
const loadError = ref('');
const busy = ref(false);
const alertText = ref('');
const notice = ref('');
const confirm = ref<'idle' | 'confirming' | 'pending' | 'done'>('idle');
const selectedTier = ref<TierNumber | null>(
  typeof route.query.tier === 'string' && /^[123]$/.test(route.query.tier) ? (Number(route.query.tier) as TierNumber) : null,
);
const selectedOffer = computed(() => tiers.value?.tiers.find((offer) => offer.tier === selectedTier.value) ?? null);
const reviewHeading = ref<HTMLElement | null>(null);

const plural = (n: number, noun: string) => `${n} ${noun}${n === 1 ? '' : 's'}`;

const liveTier = computed(() => hasLiveTier(billing.value));
// A payment past due: the tier's, a dedicated machine's or a static
// address's (any keeps new servers from starting, and no dedicated machine
// or address sells meanwhile).
const pastDue = computed(() => {
  const b = billing.value;
  if (!b) return false;
  return (
    b.subscription?.status === 'past_due' ||
    b.own_machine_subscriptions.some((s) => s.status === 'past_due') ||
    b.address_subscriptions.some((s) => s.status === 'past_due')
  );
});
// What the tier's end leaves: an admin's grant of servers keeps that many
// running on shared machines, else its servers there stop.
const afterTier = computed(() => {
  const n = billing.value?.comp_servers ?? 0;
  return n > 0 ? `your grant keeps up to ${plural(n, 'server')} running` : 'your servers on shared machines stop';
});
// A checkout came back and its confirmation is awaited: no second one starts.
const waiting = computed(() => confirm.value === 'confirming' || confirm.value === 'pending');
const currentOffer = computed(() => tiers.value?.tiers.find((t) => t.tier === billing.value?.subscription?.tier));
// The servers the account's tier covers on shared machines.
const tierServers = computed(() => currentOffer.value?.max_servers ?? billing.value?.shared_servers ?? 0);

// Why another dedicated machine cannot be bought now, or ''. The fleet's room
// is the only cap.
const ownMachineBlock = computed(() => {
  const offer = tiers.value?.own_machine;
  if (pastDue.value) return 'Fix the past-due payment first.';
  if (offer && offer.amount === null) return 'No price yet.';
  if (offer && !offer.available) return HOSTING_FULL;
  return '';
});

// What an admin's grant gives (anything bought adds to it).
const grantText = computed(() => {
  const b = billing.value;
  if (!b) return '';
  const parts: string[] = [];
  if (b.comp_servers > 0) parts.push(plural(b.comp_servers, 'shared server'));
  if (b.comp_own_machines > 0) parts.push(plural(b.comp_own_machines, 'dedicated machine'));
  if (b.comp_addresses > 0) parts.push(`${b.comp_addresses} static address${b.comp_addresses === 1 ? '' : 'es'}`);
  return parts.length > 1 ? `${parts.slice(0, -1).join(', ')} and ${parts[parts.length - 1]}` : (parts[0] ?? '');
});

// Why another static address cannot be bought now, or '' when it can: each
// is a dedicated machine's.
const addressBlock = computed(() => {
  const b = billing.value;
  const offer = tiers.value?.static_address;
  if (!b || !offer) return '';
  if (b.own_machines === 0) return 'Needs a dedicated machine first.';
  if (b.own_machines <= b.static_addresses) return 'Each dedicated machine has one already.';
  if (offer.amount === null) return 'No price yet.';
  if (!offer.available) return HOSTING_FULL;
  return '';
});

// The status line: the confirmation's progress, or the last thing done.
const statusText = computed(() => {
  if (confirm.value === 'confirming') return CONFIRMING_TEXT;
  if (confirm.value === 'pending') return PENDING_TEXT;
  return notice.value;
});

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

function fail(error: BillingError) {
  if (error.code === 'unauthorized') {
    void onSessionEnded();
    return;
  }
  alertText.value = error.message;
}

// Reads the tiers and the account: 'ok' with both in, 'closed' when billing
// is off (the page says so, no error), 'failed' otherwise.
async function load(): Promise<'ok' | 'closed' | 'failed'> {
  loading.value = true;
  loadError.value = '';
  const [t, b] = await Promise.all([fetchTiers(), fetchMyBilling()]);
  loading.value = false;
  if (!b.ok && b.error.code === 'unauthorized') {
    await onSessionEnded();
    return 'failed';
  }
  if (!t.ok) {
    loadError.value = t.error.message;
    return 'failed';
  }
  if (!b.ok && b.error.code === 'billing_disabled') {
    tiers.value = t.value;
    billing.value = null;
    return 'closed';
  }
  if (!b.ok) {
    loadError.value = b.error.message;
    return 'failed';
  }
  tiers.value = t.value;
  billing.value = b.value;
  return 'ok';
}

// "Try again": the read, then a checkout's confirmation the failed read held back.
async function retry() {
  const outcome = await load();
  if (outcome === 'ok' && route.query.checkout === 'success' && confirm.value === 'idle') void confirmCheckout();
  else if (outcome === 'closed') void dropQuery();
}

// Leaves for Stripe, or says the address looked wrong.
function go(url: string): void {
  if (!navigateToStripe(url)) alertText.value = BAD_STRIPE_URL;
}

// The billing portal; with a subscription's id, its cancel flow (a static
// address's).
async function portal(subscriptionId?: string) {
  if (busy.value) return;
  busy.value = true;
  alertText.value = '';
  const result = subscriptionId ? await openPortal(subscriptionId) : await openPortal();
  busy.value = false;
  if (!result.ok) {
    fail(result.error);
    return;
  }
  go(result.value);
}

async function checkout(item: BillingItem) {
  if (busy.value) return;
  busy.value = true;
  alertText.value = '';
  notice.value = '';
  const result = await startCheckout(item);
  busy.value = false;
  if (!result.ok) {
    // A tier is already live: it changes in the portal.
    if (result.error.code === 'already_subscribed') {
      notice.value = 'You already have a tier: opening Stripe to change it.';
      await portal();
      return;
    }
    fail(result.error);
    // An address Stripe sold meanwhile (another tab): the page catches up.
    if (result.error.code === 'address_limit') {
      const fresh = await fetchMyBilling();
      if (fresh.ok) billing.value = fresh.value;
    }
    // The fleet sold out since the page read the tiers: their cards say so.
    if (result.error.code === 'sold_out') {
      const fresh = await fetchTiers();
      if (fresh.ok) tiers.value = fresh.value;
    }
    return;
  }
  const b = billing.value;
  rememberCheckout(item, b?.own_machines ?? 0, b ? activeAddressSubscriptions(b) : 0);
  go(result.value);
}

async function onChoose(tier: TierNumber) {
  if (liveTier.value) void portal();
  else {
    selectedTier.value = tier;
    await nextTick();
    reviewHeading.value?.focus();
  }
}

// ---------------------------------------------------------------------------
// Back from Stripe.

let confirmTimer: ReturnType<typeof setTimeout> | undefined;
let confirmStarted = 0;
let confirmDelay = CONFIRM_FIRST_MS;
let pending: PendingCheckout = { item: 'tier1', ownMachinesBefore: 0, addressesBefore: 0 };
let unmounted = false;

async function dropQuery() {
  await router.replace({ path: route.path, query: {}, hash: route.hash });
}

function confirmed(value: MyBilling): boolean {
  billing.value = value;
  if (!checkoutConfirmed(value, pending)) return false;
  confirm.value = 'done';
  notice.value =
    pending.item === 'own_machine'
      ? 'Your dedicated machine is active.'
      : pending.item === 'static_address'
        ? 'Your static address is paid for: name it below.'
        : 'Your tier is active: thank you for supporting OpenNova.';
  void dropQuery();
  return true;
}

function scheduleConfirm() {
  if (unmounted) return;
  let delay: number;
  if (Date.now() - confirmStarted >= CONFIRM_WINDOW_MS) {
    confirm.value = 'pending';
    delay = CONFIRM_SLOW_MS;
  } else {
    delay = confirmDelay;
    confirmDelay = Math.min(CONFIRM_MAX_MS, Math.round(confirmDelay * 1.5));
  }
  confirmTimer = setTimeout(async () => {
    confirmTimer = undefined;
    const result = await fetchMyBilling();
    if (unmounted) return;
    if (result.ok && confirmed(result.value)) return;
    if (!result.ok && result.error.code === 'unauthorized') {
      await onSessionEnded();
      return;
    }
    if (!result.ok && result.error.code === 'billing_disabled') {
      nothingToConfirm();
      return;
    }
    scheduleConfirm();
  }, delay);
}

// The return names no payment this account could have made (it has no
// billing account, or billing closed): nothing is awaited or claimed.
function nothingToConfirm() {
  confirm.value = 'idle';
  void dropQuery();
}

async function confirmCheckout() {
  pending = recallCheckout();
  confirm.value = 'confirming';
  confirmStarted = Date.now();
  confirmDelay = CONFIRM_FIRST_MS;
  const result = await syncMyBilling();
  if (unmounted) return;
  if (result.ok && confirmed(result.value)) return;
  if (!result.ok && result.error.code === 'unauthorized') {
    await onSessionEnded();
    return;
  }
  if (!result.ok && (result.error.code === 'no_billing_account' || result.error.code === 'billing_disabled')) {
    nothingToConfirm();
    return;
  }
  scheduleConfirm();
}

onMounted(async () => {
  const outcome = route.query.checkout;
  const loaded = await load();
  if (outcome === 'success') {
    // A read that failed leaves the query for "Try again"; with billing
    // closed there is nothing to confirm.
    if (loaded === 'ok') void confirmCheckout();
    else if (loaded === 'closed') void dropQuery();
  } else if (outcome === 'cancel') {
    // The session's hold on the fleet's room goes now, not at its end.
    void cancelCheckout();
    notice.value = 'No payment was taken.';
    void dropQuery();
  }
});

onUnmounted(() => {
  unmounted = true;
  if (confirmTimer) clearTimeout(confirmTimer);
});
</script>
