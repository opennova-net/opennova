<template>
  <section v-if="shown" id="addresses" class="space-y-3" aria-labelledby="addresses-heading">
    <div class="flex flex-wrap items-center justify-between gap-3">
      <h2 id="addresses-heading" class="section-heading">Static addresses</h2>
      <div v-if="offer" class="flex flex-wrap items-center gap-3">
        <span class="text-sm text-ink-muted">{{ offer.amount === null ? 'No price yet' : `${formatMoney(offer.amount, currency)} a month each` }}</span>
        <button
          type="button"
          class="btn-secondary btn-sm"
          :disabled="busy || addBlock !== ''"
          :aria-describedby="addBlock ? 'addresses-add-block' : undefined"
          @click="emit('add')"
        >
          Add a static address
        </button>
      </div>
    </div>
    <p v-if="offer && addBlock" id="addresses-add-block" class="text-xs text-ink-muted">{{ addBlock }}</p>

    <Notice v-if="data === null && loadError" tone="bad">
      {{ loadError }} <button type="button" class="btn-ghost btn-sm" @click="poll.refresh">Try again</button>
    </Notice>

    <!-- A bought address waits for its name. -->
    <form v-if="data?.enabled && data.unnamed > 0" class="card space-y-1 p-4" novalidate @submit.prevent="submitName">
      <label for="address-name" class="field-label">Name your new address{{ data.unnamed > 1 ? ` (${data.unnamed} waiting)` : '' }}</label>
      <div class="flex flex-wrap items-center gap-2">
        <input
          id="address-name"
          v-model="name"
          type="text"
          class="field w-48 font-mono"
          placeholder="night-ops"
          autocomplete="off"
          autocapitalize="none"
          spellcheck="false"
          maxlength="32"
          :disabled="busy || naming"
          :aria-invalid="nameError ? 'true' : undefined"
          :aria-describedby="nameError ? 'address-name-error' : undefined"
        />
        <span class="break-all font-mono text-sm text-ink-muted">.{{ data.zone }}</span>
        <button type="submit" class="btn-primary" :disabled="busy || naming">Name it</button>
      </div>
      <p v-if="nameError" id="address-name-error" class="text-xs text-danger">{{ nameError }}</p>
    </form>

    <div v-if="data" class="table-wrap">
      <table class="data-table">
        <thead>
          <tr>
            <th scope="col">Name</th>
            <th scope="col">Address</th>
            <th scope="col">Server</th>
            <th scope="col">State</th>
            <th scope="col">Paid</th>
            <th scope="col"><span class="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="a in data.addresses" :id="`address-${a.id}`" :key="a.id">
            <th scope="row" class="break-all font-mono">{{ a.hostname }}</th>
            <td>
              <span v-if="addressEndpoint(a)" class="inline-flex flex-wrap items-center gap-1">
                <span class="break-all font-mono text-xs">{{ addressEndpoint(a) }}</span>
                <button type="button" class="btn-ghost btn-sm" @click="copy(a)">
                  Copy<span class="sr-only"> {{ addressEndpoint(a) }}</span>
                </button>
              </span>
              <span v-else class="text-ink-muted">Not given yet</span>
            </td>
            <td>
              <ServerLink v-if="a.server" :name="a.server.game_name" :id="a.server.id" scope="owner" />
              <span v-else class="text-ink-muted">None</span>
            </td>
            <td>
              <span :class="addressStateClass(a.state)">{{ addressStateLabel(a) }}</span>
              <p v-if="a.state === 'failed' && a.reason" class="mt-1 break-words text-xs text-danger">{{ a.reason }}</p>
            </td>
            <td class="whitespace-nowrap">{{ paidBy(a) }}</td>
            <td class="text-right">
              <button v-if="canCancel(a)" type="button" class="btn-danger btn-sm" :disabled="busy" @click="emit('portal', a.stripe_subscription_id!)">
                Cancel<span class="sr-only"> {{ a.hostname }}</span>
              </button>
            </td>
          </tr>
          <TableState v-if="data.addresses.length === 0" :colspan="6" :rows="data.addresses" empty="No static addresses yet." />
        </tbody>
      </table>
    </div>
  </section>
</template>

<script setup lang="ts">
// The account's static addresses on its Billing page (GET /api/me/addresses):
// the add (a checkout the page runs), the name a bought one waits for (POST
// /api/me/addresses), and each address with where players join it, its
// server, state and subscription; a subscription's address cancels in
// Stripe's billing portal (the page opens it). Reads again every few seconds
// while one moves on its own.
import { computed, nextTick, ref, watch } from 'vue';
import { useRoute } from 'vue-router';
import ServerLink from './links/ServerLink.vue';
import Notice from './ui/Notice.vue';
import TableState from './ui/TableState.vue';
import { fetchMyAddresses, nameAddress } from '../api/addresses';
import { usePolling } from '../composables/usePolling';
import type { Address, AddressesResponse } from '../types/addresses';
import type { AddressSubscription, MyBilling, StaticAddressOffer } from '../types/billing';
import { addressEndpoint, addressNameProblem, addressStateClass, addressStateLabel, isAddressMoving } from '../utils/addressView';
import { formatDate, formatMoney } from '../utils/format';

const MOVING_POLL_MS = 5_000;
// The states after which an address is on its way out: nothing to cancel.
const ENDING: ReadonlySet<string> = new Set(['lapsed', 'releasing', 'released']);

const props = withDefaults(
  defineProps<{
    // The account's billing, for its address subscriptions; null while unread.
    billing: MyBilling | null;
    // The add-on on sale, or null.
    offer: StaticAddressOffer | null;
    currency?: string;
    // Why another cannot be added now; '' when it can.
    addBlock?: string;
    busy?: boolean;
  }>(),
  { currency: 'usd', addBlock: '', busy: false },
);

const emit = defineEmits<{
  add: [];
  portal: [subscriptionId: string];
  notice: [text: string];
  error: [text: string];
  'session-ended': [];
}>();

const route = useRoute();
const data = ref<AddressesResponse | null>(null);
const loadError = ref('');
const naming = ref(false);
const name = ref('');
const nameError = ref('');

const shown = computed(() => props.offer !== null || (data.value !== null && (data.value.addresses.length > 0 || data.value.unnamed > 0)) || (data.value === null && loadError.value !== ''));

const subscriptions = computed(() => {
  const byId = new Map<string, AddressSubscription>();
  for (const s of props.billing?.address_subscriptions ?? []) byId.set(s.stripe_subscription_id, s);
  return byId;
});

let scrolled = false;
const poll = usePolling({
  async read(signal, background) {
    const result = await fetchMyAddresses(signal);
    if (signal.aborted) return false;
    if (!result.ok) {
      if (result.error.code === 'unauthorized') {
        emit('session-ended');
        return false;
      }
      if (!background || data.value === null) loadError.value = result.error.message;
      return false;
    }
    data.value = result.value;
    loadError.value = '';
    // A link to /me/billing#addresses lands here once the section is drawn.
    if (!scrolled && route.hash === '#addresses') {
      scrolled = true;
      await nextTick();
      document.getElementById('addresses')?.scrollIntoView?.();
    }
    return true;
  },
  next: () => (data.value?.addresses.some((a) => isAddressMoving(a.state)) ? MOVING_POLL_MS : null),
});

// A confirmed checkout adds a bought address waiting for its name.
watch(
  () => props.billing?.static_addresses,
  (now, before) => {
    if (before !== undefined && now !== before) void poll.readAgain();
  },
);

function subscriptionOf(a: Address): AddressSubscription | undefined {
  return a.stripe_subscription_id ? subscriptions.value.get(a.stripe_subscription_id) : undefined;
}

function paidBy(a: Address): string {
  if (a.source === 'comp') return 'Granted';
  const sub = subscriptionOf(a);
  if (!sub) return 'Subscription';
  return `${sub.cancel_at_period_end ? 'Ends' : 'Renews'} ${formatDate(sub.current_period_end)}`;
}

// A subscription's address while it lasts, unless its subscription is
// canceled already (or, the billing read, no longer open).
function canCancel(a: Address): boolean {
  if (a.source !== 'subscription' || !a.stripe_subscription_id || ENDING.has(a.state)) return false;
  if (props.billing === null) return true;
  const sub = subscriptionOf(a);
  return !!sub && !sub.cancel_at_period_end;
}

async function copy(a: Address) {
  const text = addressEndpoint(a);
  try {
    await navigator.clipboard.writeText(text);
    emit('notice', `Copied ${text}.`);
  } catch {
    emit('error', 'Your browser would not copy it.');
  }
}

async function submitName() {
  if (naming.value) return;
  const wanted = name.value.trim();
  nameError.value = addressNameProblem(wanted);
  if (nameError.value) return;
  naming.value = true;
  const result = await nameAddress(wanted);
  naming.value = false;
  if (!result.ok) {
    if (result.error.code === 'unauthorized') {
      emit('session-ended');
      return;
    }
    if (result.error.code === 'invalid_name' || result.error.code === 'name_taken') {
      nameError.value = result.error.message;
      return;
    }
    emit('error', result.error.message);
    void poll.readAgain();
    return;
  }
  name.value = '';
  emit('notice', `${result.value.hostname} is being set up.`);
  void poll.readAgain();
}

defineExpose({ readAgain: () => poll.readAgain() });
</script>
