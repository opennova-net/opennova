<template>
  <div class="space-y-8">
    <header>
      <h1 class="text-3xl font-semibold text-ink">Support tier</h1>
      <p class="mt-1 max-w-3xl text-sm text-ink-muted">
        A monthly support tier funds OpenNova and lets you run hosted servers on NovaWorld. It is support for the
        project, not the sale of any game. Hosted servers are pre-1.0 and experimental.
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
      <button type="button" :class="secondaryButton" @click="load">Try again</button>
    </div>

    <p v-else-if="!tiers?.enabled || !billing?.enabled" class="rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted">
      Support tiers are not open on NovaWorld yet.
    </p>

    <template v-else-if="tiers && billing">
      <!-- Past due: the servers keep running while Stripe retries. -->
      <section
        v-if="billing.subscription?.status === 'past_due'"
        class="space-y-3 rounded-panel border border-warn/40 bg-warn/10 p-4 sm:p-6"
        aria-labelledby="support-past-due"
      >
        <h2 id="support-past-due" class="text-lg font-semibold text-warn">Your last payment did not go through</h2>
        <p class="text-sm text-ink">
          Your servers keep running while Stripe retries the card; new servers can't start. Update your payment
          method to keep your tier.
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
            Tier {{ billing.subscription.tier }}, up to {{ currentOffer?.max_servers ?? billing.max_servers }} hosted
            server{{ (currentOffer?.max_servers ?? billing.max_servers) === 1 ? '' : 's' }} at once
          </dd>
          <dt class="text-ink-muted">Status</dt>
          <dd class="text-ink">{{ subscriptionStatusLabel(billing.subscription.status) }}</dd>
          <dt class="text-ink-muted">{{ billing.subscription.cancel_at_period_end ? 'Ends' : 'Renews' }}</dt>
          <dd class="text-ink">
            <time :datetime="isoTime(billing.subscription.current_period_end)" :title="formatUtc(billing.subscription.current_period_end)">
              {{ formatDate(billing.subscription.current_period_end) }}
            </time>
          </dd>
          <dt class="text-ink-muted">Servers live</dt>
          <dd class="text-ink">{{ billing.used }} of {{ billing.max_servers }}</dd>
        </dl>
        <p v-if="billing.subscription.cancel_at_period_end" class="text-sm text-warn">
          Your tier is canceled: it lasts until the end of the period you paid for, then your hosted servers stop.
          You can renew it in the billing portal before then.
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
          Changes and cancellations happen in Stripe's billing portal. A cancellation takes effect at the end of the
          period you paid for: your servers keep running until then.
        </p>
      </section>

      <p v-if="billing.comp_servers > 0" class="rounded-panel border border-border bg-panel p-4 text-sm text-ink">
        An admin granted your account {{ billing.comp_servers }} hosted server{{ billing.comp_servers === 1 ? '' : 's' }}.
        A support tier covering more raises your limit to the tier's.
      </p>

      <section class="space-y-4" aria-labelledby="support-tiers">
        <h2 id="support-tiers" class="text-xl font-semibold text-ink">{{ liveTier ? 'Change your tier' : 'Pick a tier' }}</h2>
        <p v-if="!liveTier && !billing.can_subscribe" class="text-sm text-warn">Your account cannot subscribe right now.</p>
        <TierPicker
          :tiers="tiers.tiers"
          :currency="tiers.currency"
          :mode="liveTier ? 'manage' : 'subscribe'"
          :current-tier="billing.subscription?.tier ?? null"
          :used="billing.used"
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

      <!-- The own-machine add-on. -->
      <section v-if="tiers.own_machine" class="space-y-3 rounded-panel border border-border bg-panel p-4 sm:p-6" aria-labelledby="support-own-machine">
        <div class="flex flex-col gap-1 sm:flex-row sm:items-baseline sm:justify-between">
          <h2 id="support-own-machine" class="text-lg font-semibold text-ink">Own machine add-on</h2>
          <p class="text-ink">
            <template v-if="tiers.own_machine.amount !== null">
              <span class="text-xl font-semibold">{{ formatMoney(tiers.own_machine.amount, tiers.currency) }}</span>
              <span class="text-sm text-ink-muted"> a month for each</span>
            </template>
            <span v-else class="text-sm text-ink-muted">Price not set yet</span>
          </p>
        </div>
        <p class="text-sm text-ink-muted">
          Runs one of your tier's servers alone on a machine of its own instead of a shared one. It adds no servers:
          each own machine holds one of the servers your tier covers.
        </p>
        <template v-if="billing.own_machine_subscription">
          <p class="text-sm text-ink">
            You have {{ billing.own_machine_subscription.quantity }} own machine{{ billing.own_machine_subscription.quantity === 1 ? '' : 's' }}
            ({{ subscriptionStatusLabel(billing.own_machine_subscription.status).toLowerCase() }}).
          </p>
          <button type="button" :disabled="busy || !billing.can_manage" :class="secondaryButton" @click="portal">
            Change how many, or cancel
          </button>
        </template>
        <template v-else>
          <button
            type="button"
            :disabled="busy || !billing.subscription?.entitled || tiers.own_machine.amount === null"
            :class="primaryButton"
            aria-describedby="support-own-machine-hint"
            @click="checkout('own_machine')"
          >
            Add an own machine
          </button>
          <p id="support-own-machine-hint" class="text-xs text-ink-muted">
            {{ billing.subscription?.entitled ? 'You can change how many in the billing portal later.' : 'Needs a support tier first.' }}
          </p>
        </template>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /me/support: the account's support tier (GET /api/me/billing) and the tiers
// on offer (GET /api/billing/tiers). With no tier, Subscribe starts a Stripe
// Checkout (POST /api/billing/checkout); with one, every change, the payment
// method and the cancellation open Stripe's billing portal (POST
// /api/billing/portal); a checkout for a tier the account already has
// (already_subscribed) opens the portal too. Both leave through
// navigateToStripe. Back from a checkout (?checkout=success), the page asks
// the service to read Stripe now (POST /api/me/billing/sync), then reads the
// account every 2 s backing off to 5 s for a minute until the payment is
// confirmed, then every 30 s with a note that it went through, and drops the
// query once confirmed; ?checkout=cancel says no payment was taken.
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
  checkoutConfirmed,
  hasLiveTier,
  recallCheckoutItem,
  rememberCheckoutItem,
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
// A checkout came back and its confirmation is awaited: no second one starts.
const waiting = computed(() => confirm.value === 'confirming' || confirm.value === 'pending');
const currentOffer = computed(() => tiers.value?.tiers.find((t) => t.tier === billing.value?.subscription?.tier));

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

async function load() {
  loading.value = true;
  loadError.value = '';
  const [t, b] = await Promise.all([fetchTiers(), fetchMyBilling()]);
  loading.value = false;
  if (!b.ok && b.error.code === 'unauthorized') {
    await onSessionEnded();
    return;
  }
  if (!t.ok || !b.ok) {
    loadError.value = (!t.ok ? t.error : (b as { ok: false; error: BillingError }).error).message;
    return;
  }
  tiers.value = t.value;
  billing.value = b.value;
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
    return;
  }
  rememberCheckoutItem(item);
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
let confirmItem: BillingItem = 'tier1';
let unmounted = false;

async function dropQuery() {
  await router.replace({ path: route.path, query: {} });
}

function confirmed(value: MyBilling): boolean {
  billing.value = value;
  if (!checkoutConfirmed(value, confirmItem)) return false;
  confirm.value = 'done';
  notice.value =
    confirmItem === 'own_machine'
      ? 'Your own machine is active. Start a server on it from your Servers page.'
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
    scheduleConfirm();
  }, delay);
}

async function confirmCheckout() {
  confirmItem = recallCheckoutItem();
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
  scheduleConfirm();
}

onMounted(async () => {
  const outcome = route.query.checkout;
  await load();
  if (outcome === 'success') {
    void confirmCheckout();
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
