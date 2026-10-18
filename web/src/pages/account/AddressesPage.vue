<template>
  <div class="space-y-8">
    <header>
      <h1 class="text-3xl font-semibold text-ink">Static addresses</h1>
      <p class="mt-1 max-w-3xl text-sm text-ink-muted">
        A static address gives a server on one of your own machines the same IP address and a name of its own every
        time it starts, so players find it in the same place. Add one on your
        <RouterLink to="/me/support" class="text-accent hover:text-ink-bright">Support tier</RouterLink> page.
      </p>
    </header>

    <!-- Live regions, always in the page so a screen reader hears what lands
         in them; the boxes below are their visible copies. -->
    <p role="status" class="sr-only">{{ notice }}</p>
    <p role="alert" class="sr-only">{{ alertText }}</p>
    <p v-if="notice" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">
      {{ notice }}
    </p>
    <p v-if="alertText" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      {{ alertText }}
    </p>

    <div v-if="data === null && !loadError" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading your static addresses…
    </div>

    <div v-else-if="data === null" class="space-y-3 rounded-panel border border-border bg-panel p-6">
      <p class="text-sm text-danger">{{ loadError }}</p>
      <button type="button" :class="secondaryButton" @click="poll.refresh">Try again</button>
    </div>

    <template v-else>
      <p v-if="!data.enabled && data.addresses.length === 0" class="rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted">
        NovaWorld does not offer static addresses right now.
      </p>

      <!-- A bought address waits for its name. -->
      <section
        v-if="data.enabled && data.unnamed > 0"
        class="space-y-3 rounded-panel border border-accent/40 bg-panel p-4 sm:p-6"
        aria-labelledby="addresses-name"
      >
        <h2 id="addresses-name" class="text-lg font-semibold text-ink">Name your new address</h2>
        <p class="text-sm text-ink-muted">
          {{ data.unnamed === 1 ? 'One static address you bought waits' : `${data.unnamed} static addresses you bought wait` }}
          for a name. The name becomes its host name under {{ data.zone }} and stays with it.
        </p>
        <form class="space-y-2" novalidate @submit.prevent="submitName">
          <label for="address-name" :class="labelClass">Name</label>
          <div class="flex flex-wrap items-center gap-2">
            <input
              id="address-name"
              v-model="name"
              type="text"
              autocomplete="off"
              autocapitalize="none"
              spellcheck="false"
              maxlength="32"
              :disabled="busy"
              aria-describedby="address-name-hint"
              :aria-invalid="nameError ? 'true' : undefined"
              class="w-48 rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink focus:border-accent focus:outline-none disabled:opacity-60 aria-[invalid=true]:border-danger"
            />
            <span class="break-all font-mono text-sm text-ink-muted">.{{ data.zone }}</span>
            <button type="submit" :disabled="busy" :class="primaryButton">Name it</button>
          </div>
          <p id="address-name-hint" class="text-xs" :class="nameError ? 'text-danger' : 'text-ink-muted'">
            {{ nameError || ADDRESS_NAME_RULE }}
          </p>
        </form>
      </section>

      <p
        v-if="data.enabled && data.addresses.length === 0 && data.unnamed === 0"
        class="rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted"
      >
        You have no static address yet.
        <RouterLink to="/me/support" class="text-accent hover:text-ink-bright">Add one on your Support tier page</RouterLink>.
      </p>

      <ul v-if="data.addresses.length > 0" class="space-y-4">
        <li v-for="a in data.addresses" :key="a.id">
          <article
            class="space-y-3 rounded-panel border border-border bg-panel p-4 sm:p-5"
            :aria-labelledby="`address-${a.id}-heading`"
          >
            <div class="flex flex-col gap-1 sm:flex-row sm:items-baseline sm:justify-between">
              <h2 :id="`address-${a.id}-heading`" class="break-all font-mono text-lg font-semibold text-ink">{{ a.hostname }}</h2>
              <span class="text-sm font-medium" :class="addressStateClass(a.state)">{{ addressStateLabel(a.state) }}</span>
            </div>
            <p v-if="a.state === 'failed' && a.reason" class="break-words text-sm text-danger">{{ a.reason }}</p>
            <dl class="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 text-sm">
              <dt class="text-ink-muted">Address</dt>
              <dd class="flex flex-wrap items-center gap-2 text-ink">
                <template v-if="addressEndpoint(a)">
                  <span class="break-all font-mono text-xs">{{ addressEndpoint(a) }}</span>
                  <button type="button" class="text-xs text-accent hover:text-ink-bright" @click="copy(a)">
                    Copy<span class="sr-only"> the address {{ addressEndpoint(a) }}</span>
                  </button>
                </template>
                <span v-else class="text-ink-muted">Not given yet</span>
              </dd>
              <dt class="text-ink-muted">Server</dt>
              <dd class="min-w-0 text-ink">
                <RouterLink v-if="a.server" :to="`/me/servers/${a.server.id}`" class="break-words text-accent hover:text-ink-bright">
                  {{ a.server.game_name || 'Unnamed server' }}
                </RouterLink>
                <span v-else class="text-ink-muted">None runs at it now</span>
              </dd>
              <dt class="text-ink-muted">Paid by</dt>
              <dd class="text-ink">{{ paidBy(a) }}</dd>
            </dl>
            <div v-if="canCancel(a)" class="space-y-1">
              <button
                type="button"
                :disabled="busy"
                :class="dangerButton"
                :aria-describedby="`address-${a.id}-cancel-hint`"
                @click="cancel(a)"
              >
                Cancel<span class="sr-only"> the subscription of {{ a.hostname }}</span>
              </button>
              <p :id="`address-${a.id}-cancel-hint`" class="text-xs text-ink-muted">
                Cancel its subscription in Stripe's billing portal. The address stays until the period you paid for
                ends; then it is released, and a server running at it moves to another address.
              </p>
            </div>
          </article>
        </li>
      </ul>
    </template>
  </div>
</template>

<script setup lang="ts">
// /me/addresses: the account's static addresses (GET /api/me/addresses), each
// with its host name, the IP and port players join at (with a copy), its state
// in words and the server running at it. A bought address waits for its name
// (POST /api/me/addresses, checked here first by the service's rule); a
// subscription's address is canceled in Stripe's billing portal, whose cancel
// flow for that subscription (POST /api/billing/portal {cancel_subscription})
// returns here; an admin's grant has no cancel. The account's billing (GET
// /api/me/billing) says which subscriptions are canceled already. The page
// reads again every few seconds while an address is being set up or released.
import { computed, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import { fetchMyAddresses, nameAddress } from '../../api/addresses';
import { fetchMyBilling, openPortal } from '../../api/billing';
import { usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import type { Address, AddressesResponse } from '../../types/addresses';
import type { AddressSubscription, MyBilling } from '../../types/billing';
import {
  ADDRESS_NAME_RULE,
  addressEndpoint,
  addressNameProblem,
  addressStateClass,
  addressStateLabel,
  isAddressMoving,
} from '../../utils/addressView';
import { subscriptionStatusLabel } from '../../utils/billingView';
import { formatDate } from '../../utils/format';
import { navigateToStripe } from '../../utils/navigate';

const MOVING_POLL_MS = 5_000;
// The states after which an address is on its way out: nothing to cancel.
const ENDING: ReadonlySet<string> = new Set(['lapsed', 'releasing', 'released']);

const labelClass = 'block text-xs font-medium uppercase tracking-wider text-ink-muted';
const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-40';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';
const dangerButton =
  'rounded-control border border-danger/60 px-4 py-2 text-sm font-medium text-danger transition hover:bg-danger/10 disabled:cursor-not-allowed disabled:opacity-40';

const route = useRoute();
const router = useRouter();
const { refresh: refreshSession } = useSession();

const data = ref<AddressesResponse | null>(null);
// The account's billing, for its address subscriptions; null while unread (or
// billing is off), when every subscription's address offers its cancel.
const billing = ref<MyBilling | null>(null);
const loadError = ref('');
const busy = ref(false);
const notice = ref('');
const alertText = ref('');
const name = ref('');
const nameError = ref('');

const subscriptions = computed(() => {
  const byId = new Map<string, AddressSubscription>();
  for (const s of billing.value?.address_subscriptions ?? []) byId.set(s.stripe_subscription_id, s);
  return byId;
});

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

const poll = usePolling({
  async read(signal, background) {
    const [addresses, mine] = await Promise.all([fetchMyAddresses(signal), fetchMyBilling(signal)]);
    if (signal.aborted) return false;
    if (!addresses.ok) {
      if (addresses.error.code === 'unauthorized') {
        await onSessionEnded();
        return false;
      }
      if (!background || data.value === null) loadError.value = addresses.error.message;
      return false;
    }
    data.value = addresses.value;
    loadError.value = '';
    if (mine.ok) billing.value = mine.value;
    return true;
  },
  next: () => (data.value?.addresses.some((a) => isAddressMoving(a.state)) ? MOVING_POLL_MS : null),
});

function subscriptionOf(a: Address): AddressSubscription | undefined {
  return a.stripe_subscription_id ? subscriptions.value.get(a.stripe_subscription_id) : undefined;
}

function paidBy(a: Address): string {
  if (a.source === 'comp') return 'Granted by an admin';
  const sub = subscriptionOf(a);
  if (!sub) return 'Its own subscription';
  if (sub.cancel_at_period_end) return `Its own subscription, canceled: it ends on ${formatDate(sub.current_period_end)}`;
  return `Its own subscription (${subscriptionStatusLabel(sub.status).toLowerCase()}), renews on ${formatDate(sub.current_period_end)}`;
}

// A subscription's address while it lasts, unless its subscription is
// canceled already (or, the billing read, no longer open).
function canCancel(a: Address): boolean {
  if (a.source !== 'subscription' || !a.stripe_subscription_id || ENDING.has(a.state)) return false;
  if (billing.value === null) return true;
  const sub = subscriptionOf(a);
  return !!sub && !sub.cancel_at_period_end;
}

async function copy(a: Address) {
  const text = addressEndpoint(a);
  notice.value = '';
  alertText.value = '';
  try {
    await navigator.clipboard.writeText(text);
    notice.value = `Copied ${text}.`;
  } catch {
    alertText.value = 'Your browser would not copy it. Select the address and copy it instead.';
  }
}

async function submitName() {
  if (busy.value) return;
  const wanted = name.value.trim();
  notice.value = '';
  alertText.value = '';
  nameError.value = addressNameProblem(wanted);
  if (nameError.value) return;
  busy.value = true;
  const result = await nameAddress(wanted);
  busy.value = false;
  if (!result.ok) {
    if (result.error.code === 'unauthorized') {
      void onSessionEnded();
      return;
    }
    if (result.error.code === 'invalid_name' || result.error.code === 'name_taken') {
      nameError.value = result.error.message;
      return;
    }
    alertText.value = result.error.message;
    void poll.readAgain();
    return;
  }
  name.value = '';
  notice.value = `${result.value.hostname} is being set up. It is ready in a minute or two.`;
  void poll.readAgain();
}

async function cancel(a: Address) {
  if (busy.value || !a.stripe_subscription_id) return;
  busy.value = true;
  notice.value = '';
  alertText.value = '';
  const result = await openPortal(a.stripe_subscription_id);
  busy.value = false;
  if (!result.ok) {
    if (result.error.code === 'unauthorized') {
      void onSessionEnded();
      return;
    }
    alertText.value = result.error.message;
    if (result.error.code === 'not_found') void poll.readAgain();
    return;
  }
  if (!navigateToStripe(result.value)) {
    alertText.value = "Stripe's address in the reply looked wrong, so this page did not follow it. Try again.";
  }
}
</script>
