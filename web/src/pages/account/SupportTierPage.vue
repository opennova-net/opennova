<template>
  <div class="space-y-8">
    <header>
      <h1 class="text-3xl font-semibold text-ink">Support tier</h1>
      <p class="mt-1 max-w-3xl text-sm text-ink-muted">
        A monthly tier funds OpenNova and lets you run hosted servers on shared machines; a dedicated machine adds one
        more on a machine of its own, with or without a tier. It supports the project; it is not the sale of a game.
        Hosted servers are pre-1.0 and experimental.
      </p>
    </header>

    <!-- Live regions, always in the page so a screen reader hears what lands
         in them; the boxes below are their visible copies. -->
    <p role="status" class="sr-only">{{ statusText }}</p>
    <p role="alert" class="sr-only">{{ alertText }}</p>

    <p v-if="confirm === 'confirming'" aria-hidden="true" class="rounded-panel border border-border bg-panel p-4 text-sm text-ink">
      Confirming your payment with Stripe…
    </p>
    <p v-else-if="confirm === 'pending'" aria-hidden="true" class="rounded-panel border border-warn/40 bg-warn/10 p-4 text-sm text-warn">
      {{ PENDING_TEXT }}
    </p>
    <p
      v-if="statusText && !waiting"
      aria-hidden="true"
      class="rounded-panel border p-4 text-sm"
      :class="confirm === 'done' ? 'border-online/40 bg-online/10 text-online' : 'border-border bg-panel text-ink'"
    >
      {{ statusText }}
    </p>
    <p v-if="alertText" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      {{ alertText }}
    </p>

    <div v-if="loading" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading the support tiers…
    </div>

    <div v-else-if="loadError" class="space-y-3 rounded-panel border border-border bg-panel p-6">
      <p class="text-sm text-danger">{{ loadError }}</p>
      <button type="button" :class="secondaryButton" @click="retry">Try again</button>
    </div>

    <p v-else-if="!tiers?.enabled || !billing?.enabled" class="rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted">
      Support tiers are not open on NovaWorld yet.
    </p>

    <template v-else-if="tiers && billing">
      <!-- Past due (the tier's payment, a dedicated machine's or a static
           address's): the servers keep running while Stripe retries. -->
      <section
        v-if="pastDue"
        class="space-y-3 rounded-panel border border-warn/40 bg-warn/10 p-4 sm:p-6"
        aria-labelledby="support-past-due"
      >
        <h2 id="support-past-due" class="text-lg font-semibold text-warn">Your last payment did not go through</h2>
        <p class="text-sm text-ink">
          Your servers keep running while Stripe retries the card; new servers can't start. Update your payment
          method to keep your subscriptions.
        </p>
        <button type="button" :disabled="busy" :class="primaryButton" @click="portal">Update your payment method</button>
      </section>

      <!-- The account's tier, while it has one. -->
      <section
        v-if="liveTier && billing.subscription"
        class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6"
        aria-labelledby="support-current"
      >
        <h2 id="support-current" class="text-lg font-semibold text-ink">Your support tier</h2>
        <dl class="grid gap-x-6 gap-y-2 text-sm sm:grid-cols-[auto_1fr]">
          <dt class="text-ink-muted">Tier</dt>
          <dd class="text-ink">
            Tier {{ billing.subscription.tier }}, up to {{ tierServers }} hosted server{{ tierServers === 1 ? '' : 's' }}
            at once on shared machines
          </dd>
          <dt class="text-ink-muted">Status</dt>
          <dd class="text-ink">{{ subscriptionStatusLabel(billing.subscription.status) }}</dd>
          <dt class="text-ink-muted">{{ billing.subscription.cancel_at_period_end ? 'Ends' : 'Renews' }}</dt>
          <dd class="text-ink">
            <time :datetime="isoTime(billing.subscription.current_period_end)" :title="formatUtc(billing.subscription.current_period_end)">
              {{ formatDate(billing.subscription.current_period_end) }}
            </time>
          </dd>
          <dt class="text-ink-muted">Live on shared machines</dt>
          <dd class="text-ink">{{ billing.shared_used }} of {{ billing.shared_servers }}</dd>
        </dl>
        <p v-if="billing.subscription.cancel_at_period_end" id="support-canceled" class="text-sm text-warn">
          Your tier is canceled: it lasts until the end of the period you paid for, then {{ afterTier }}. You can
          renew it in the billing portal before then.{{ dedicatedStay }}
        </p>
        <div class="flex flex-wrap gap-2">
          <button type="button" :disabled="busy || !billing.can_manage" :class="secondaryButton" @click="portal">
            Manage billing
          </button>
          <button
            v-if="!billing.subscription.cancel_at_period_end"
            type="button"
            :disabled="busy || !billing.can_manage"
            :class="dangerButton"
            aria-describedby="support-cancel-hint"
            @click="portal"
          >
            Cancel your tier
          </button>
        </div>
        <p v-if="!billing.subscription.cancel_at_period_end" id="support-cancel-hint" class="text-xs text-ink-muted">
          Change or cancel in Stripe's billing portal. A canceled tier lasts until the paid period ends, then
          {{ afterTier }}.{{ dedicatedStay }}
        </p>
      </section>

      <!-- An admin's grant: the account's whole hosting, with nothing to buy beside it. -->
      <p v-if="billing.granted" id="support-granted" class="rounded-panel border border-border bg-panel p-4 text-sm text-ink">
        {{ grantText }}
      </p>

      <section v-if="!billing.granted" class="space-y-4" aria-labelledby="support-tiers">
        <h2 id="support-tiers" class="text-xl font-semibold text-ink">{{ liveTier ? 'Change your tier' : 'Pick a tier' }}</h2>
        <p v-if="!liveTier && !billing.can_subscribe" class="text-sm text-warn">Your account cannot subscribe right now.</p>
        <TierPicker
          :tiers="tiers.tiers"
          :currency="tiers.currency"
          :mode="liveTier ? 'manage' : 'subscribe'"
          :current-tier="billing.subscription?.tier ?? null"
          :used="billing.shared_used - billing.shared_stopping"
          :busy="busy || waiting"
          :disabled="liveTier ? !billing.can_manage : !billing.can_subscribe"
          base-id="support"
          @choose="onChoose"
        />
        <p class="text-xs text-ink-muted">
          Prices are monthly in {{ tiers.currency.toUpperCase() }} and renew each month until you cancel. Payment goes
          through Stripe; NovaWorld never sees your card. Read the
          <RouterLink to="/terms" class="text-accent hover:text-ink-bright">terms of service</RouterLink> and the
          <RouterLink to="/refunds" class="text-accent hover:text-ink-bright">refund policy</RouterLink> before you
          subscribe.
        </p>
      </section>

      <!-- Dedicated machines: subscriptions of their own, no tier needed. -->
      <section v-if="tiers.own_machine && !billing.granted" class="space-y-3 rounded-panel border border-border bg-panel p-4 sm:p-6" aria-labelledby="support-own-machine">
        <div class="flex flex-col gap-1 sm:flex-row sm:items-baseline sm:justify-between">
          <h2 id="support-own-machine" class="text-lg font-semibold text-ink">Dedicated machines</h2>
          <p class="text-ink">
            <template v-if="tiers.own_machine.amount !== null">
              <span class="text-xl font-semibold">{{ formatMoney(tiers.own_machine.amount, tiers.currency) }}</span>
              <span class="text-sm text-ink-muted"> a month for each</span>
            </template>
            <span v-else class="text-sm text-ink-muted">Price not set yet</span>
          </p>
        </div>
        <p class="text-sm text-ink-muted">
          One more hosted server, on a machine of its own. No support tier needed; add as many as you like, each its
          own monthly subscription.
        </p>
        <template v-if="billing.own_machine_subscriptions.length > 0">
          <p class="text-sm text-ink">You have {{ ownMachines }} dedicated machine{{ ownMachines === 1 ? '' : 's' }}.</p>
          <ul id="support-own-machine-subscriptions" class="space-y-1 text-sm text-ink">
            <li v-for="s in billing.own_machine_subscriptions" :key="s.stripe_subscription_id">
              {{ s.quantity }} dedicated machine{{ s.quantity === 1 ? '' : 's' }}:
              {{ subscriptionStatusLabel(s.status) }}, {{ s.cancel_at_period_end ? 'ends' : 'renews' }}
              <time :datetime="isoTime(s.current_period_end)" :title="formatUtc(s.current_period_end)">
                {{ formatDate(s.current_period_end) }}
              </time>
            </li>
          </ul>
        </template>
        <div class="flex flex-wrap gap-2">
          <button
            type="button"
            :disabled="busy || waiting || pastDue || tiers.own_machine.amount === null || !tiers.own_machine.available"
            :class="billing.own_machine_subscriptions.length > 0 ? secondaryButton : primaryButton"
            aria-describedby="support-own-machine-hint"
            @click="checkout('own_machine')"
          >
            {{ billing.own_machine_subscriptions.length > 0 ? 'Add another' : 'Add a dedicated machine' }}
          </button>
          <button
            v-if="billing.own_machine_subscriptions.length > 0"
            type="button"
            :disabled="busy || !billing.can_manage"
            :class="secondaryButton"
            aria-describedby="support-own-machine-hint"
            @click="portal"
          >
            Cancel one in the billing portal
          </button>
        </div>
        <p id="support-own-machine-hint" class="text-xs text-ink-muted">{{ ownMachineHint }}</p>
      </section>

      <!-- The static address add-on, while the deployment sells them. -->
      <section
        v-if="tiers.static_address && !billing.granted"
        class="space-y-3 rounded-panel border border-border bg-panel p-4 sm:p-6"
        aria-labelledby="support-static-address"
      >
        <div class="flex flex-col gap-1 sm:flex-row sm:items-baseline sm:justify-between">
          <h2 id="support-static-address" class="text-lg font-semibold text-ink">Static address add-on</h2>
          <p class="text-ink">
            <template v-if="tiers.static_address.amount !== null">
              <span class="text-xl font-semibold">{{ formatMoney(tiers.static_address.amount, tiers.currency) }}</span>
              <span class="text-sm text-ink-muted"> a month for each</span>
            </template>
            <span v-else class="text-sm text-ink-muted">Price not set yet</span>
          </p>
        </div>
        <p class="text-sm text-ink-muted">
          Gives a server on one of your dedicated machines the same IP address and a name of its own every time it
          starts, so players find it in the same place. One for each dedicated machine; each is its own monthly
          subscription. With fewer dedicated machines than addresses, the ones past them are set to end with their paid
          period.
        </p>
        <p v-if="addressCount > 0" class="text-sm text-ink">
          You have {{ addressCount }} static address{{ addressCount === 1 ? '' : 'es' }}.
        </p>
        <div class="flex flex-wrap items-center gap-x-4 gap-y-2">
          <button
            type="button"
            :disabled="busy || waiting || addressBlock !== ''"
            :class="addressCount > 0 ? secondaryButton : primaryButton"
            aria-describedby="support-static-address-hint"
            @click="checkout('static_address')"
          >
            Add a static address
          </button>
          <RouterLink to="/me/addresses" class="text-sm text-accent hover:text-ink-bright">Your static addresses</RouterLink>
        </div>
        <p id="support-static-address-hint" class="text-xs text-ink-muted">{{ addressBlock || ADDRESS_HINT }}</p>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /me/support: the account's support tier (GET /api/me/billing) and the tiers
// on offer (GET /api/billing/tiers), its dedicated machines (one more server
// each, no tier needed; each checkout another subscription, listed with its
// status and renewal) and the static addresses (each a checkout of its own;
// an item the fleet is sold out of says so and cannot be bought, nor a
// dedicated machine or an address while a payment is past due). With no
// tier, Subscribe starts a Stripe Checkout (POST /api/billing/checkout); with
// one, every change, the payment method and the cancellation open Stripe's
// billing portal (POST
// /api/billing/portal); a checkout for a tier the account already has
// (already_subscribed) opens the portal too. Both leave through
// navigateToStripe. Back from a checkout (?checkout=success), the page asks
// the service to read Stripe now (POST /api/me/billing/sync), then reads the
// account every 2 s backing off to 5 s for a minute until the payment is
// confirmed, then every 30 s with a note that it went through, and drops the
// query once confirmed. A sync that finds no billing account (or support
// tiers closed) drops the query with nothing claimed, and a page whose read
// failed holds the confirmation back until "Try again". ?checkout=cancel says
// no payment was taken. With billing off the page says support tiers are not
// open. A payment past due (the tier's or a dedicated machine's or a static
// address's) shows a banner to the portal; the tier box counts the servers on
// shared machines and says what the tier's end leaves (an admin's grant of
// servers keeps them running; the dedicated machines need no tier).
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import TierPicker from '../../components/TierPicker.vue';
import {
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

const PENDING_TEXT =
  'Your payment went through, and Stripe has not confirmed it to NovaWorld yet. This page keeps checking every 30 seconds.';
const ADDRESS_HINT = 'Name it on your Static addresses page once it is paid for, and cancel it there.';

const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-40';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';
const dangerButton =
  'rounded-control border border-danger/60 px-4 py-2 text-sm font-medium text-danger transition hover:bg-danger/10 disabled:cursor-not-allowed disabled:opacity-40';

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
// running on shared machines (an account granted after it subscribed keeps
// its tier box), else its servers there stop.
const afterTier = computed(() => {
  const n = billing.value?.comp_servers ?? 0;
  return n > 0
    ? `your admin grant keeps up to ${n} hosted server${n === 1 ? '' : 's'} running on shared machines`
    : 'your hosted servers on shared machines stop';
});
// What the tier's end does not touch: the dedicated machines, subscriptions
// of their own that need no tier.
const dedicatedStay = computed(() =>
  (billing.value?.own_machines ?? 0) > 0 ? ' Your dedicated machines keep running: they need no tier.' : '',
);
// A checkout came back and its confirmation is awaited: no second one starts.
const waiting = computed(() => confirm.value === 'confirming' || confirm.value === 'pending');
const currentOffer = computed(() => tiers.value?.tiers.find((t) => t.tier === billing.value?.subscription?.tier));
// The servers the account's tier covers on shared machines.
const tierServers = computed(() => currentOffer.value?.max_servers ?? billing.value?.shared_servers ?? 0);
// The dedicated machines the account has: what its subscriptions grant now,
// else what the listed ones pay for (none granting yet, say a first payment
// still awaited).
const ownMachines = computed(() => {
  const b = billing.value;
  if (!b) return 0;
  return b.own_machines > 0 ? b.own_machines : b.own_machine_subscriptions.reduce((n, s) => n + s.quantity, 0);
});
// Why another dedicated machine cannot be bought now, or what buying one
// does. The fleet's room is the only cap.
const ownMachineHint = computed(() => {
  const offer = tiers.value?.own_machine;
  if (pastDue.value) return 'Fix the payment that is past due first, then add one.';
  if (offer && offer.amount === null) return 'Its price is not set yet.';
  if (offer && !offer.available) return HOSTING_FULL;
  if ((billing.value?.own_machine_subscriptions.length ?? 0) > 0) {
    return 'Each one you add is another subscription. Cancel one in the billing portal; it lasts until its paid period ends.';
  }
  return 'Start a server on it from your Servers page once it is paid for. Cancel it in the billing portal.';
});

// What an admin's grant gives, said whole (its servers on shared machines and
// its dedicated machines add up): an account with one buys no tier,
// dedicated machine or static address (the checkout refuses one,
// not_eligible).
const grantText = computed(() => {
  const b = billing.value;
  if (!b) return '';
  const parts: string[] = [];
  if (b.comp_servers > 0) {
    parts.push(`${b.comp_servers} hosted server${b.comp_servers === 1 ? '' : 's'} on shared machines`);
  }
  if (b.comp_own_machines > 0) {
    const one = b.comp_own_machines === 1;
    parts.push(`${b.comp_own_machines} dedicated machine${one ? '' : 's'} (${one ? 'one more server' : 'one more server each'})`);
  }
  if (b.comp_addresses > 0) parts.push(`${b.comp_addresses} static address${b.comp_addresses === 1 ? '' : 'es'}`);
  const granted = parts.length > 1 ? `${parts.slice(0, -1).join(', ')} and ${parts[parts.length - 1]}` : (parts[0] ?? '');
  return (
    `An admin granted your account ${granted}, free of charge. ` +
    'With an admin grant there is no support tier, dedicated machine or static address to buy.'
  );
});

// The static addresses the account has: one subscription each, and an
// admin's comp addresses.
const addressCount = computed(
  () => (billing.value?.address_subscriptions.length ?? 0) + (billing.value?.comp_addresses ?? 0),
);
// Why another static address cannot be bought now, or '' when it can: each
// is a dedicated machine's, so the account needs a dedicated machine without one.
const addressBlock = computed(() => {
  const b = billing.value;
  const offer = tiers.value?.static_address;
  if (!b || !offer) return '';
  if (b.own_machines === 0) return 'Needs a dedicated machine first: a static address is for a server on one.';
  if (b.own_machines <= b.static_addresses) {
    return 'You have a static address for every dedicated machine. Add a dedicated machine first to add another address.';
  }
  if (offer.amount === null) return 'Its price is not set yet.';
  if (!offer.available) return HOSTING_FULL;
  return '';
});

// The status line: the confirmation's progress, or the last thing done.
const statusText = computed(() => {
  if (confirm.value === 'confirming') return 'Confirming your payment with Stripe…';
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

// Reads the tiers and the account: 'ok' with both in, 'closed' when support
// tiers are not open (billing off: the page says so, no error), 'failed'
// otherwise.
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
  if (!navigateToStripe(url)) {
    alertText.value = "Stripe's address in the reply looked wrong, so this page did not follow it. Try again.";
  }
}

async function portal() {
  if (busy.value) return;
  busy.value = true;
  alertText.value = '';
  const result = await openPortal();
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
      notice.value = 'You already have a support tier, so the billing portal opens to change it.';
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

function onChoose(tier: TierNumber) {
  if (liveTier.value) void portal();
  else void checkout(tierItem(tier));
}

// ---------------------------------------------------------------------------
// Back from Stripe.

let confirmTimer: ReturnType<typeof setTimeout> | undefined;
let confirmStarted = 0;
let confirmDelay = CONFIRM_FIRST_MS;
let pending: PendingCheckout = { item: 'tier1', ownMachinesBefore: 0, addressesBefore: 0 };
let unmounted = false;

async function dropQuery() {
  await router.replace({ path: route.path, query: {} });
}

function confirmed(value: MyBilling): boolean {
  billing.value = value;
  if (!checkoutConfirmed(value, pending)) return false;
  confirm.value = 'done';
  notice.value =
    pending.item === 'own_machine'
      ? 'Your dedicated machine is active. Start a server on it from your Servers page.'
      : pending.item === 'static_address'
        ? 'Your static address is paid for. Give it a name on your Static addresses page.'
        : 'Your support tier is active. Thank you for supporting OpenNova.';
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
// billing account, or support tiers closed): nothing is awaited or claimed.
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
    // A read that failed leaves the query for "Try again"; with tiers closed
    // there is nothing to confirm.
    if (loaded === 'ok') void confirmCheckout();
    else if (loaded === 'closed') void dropQuery();
  } else if (outcome === 'cancel') {
    notice.value = 'No payment was taken.';
    void dropQuery();
  }
});

onUnmounted(() => {
  unmounted = true;
  if (confirmTimer) clearTimeout(confirmTimer);
});
</script>
