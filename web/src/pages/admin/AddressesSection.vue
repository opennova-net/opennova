<template>
  <div class="space-y-10">
    <AdminPageHeader
      title="Static addresses"
      :description="`The fixed addresses of own machines, each named under ${data?.zone || 'the play zone'}.`"
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="poll.refresh"
    />

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them, each its own box while it holds something: one copy of the text,
         so the page (and its text) says it once. -->
    <div role="status" :class="message ? 'rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online' : 'sr-only'">
      {{ message }}
    </div>
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">
      {{ error }}
    </div>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>

    <div v-if="data" class="space-y-1">
      <p id="addresses-held" class="text-sm text-ink">
        {{ data.held }} of {{ data.sellable }} address{{ data.sellable === 1 ? '' : 'es' }} the deployment sells held.
      </p>
      <p v-if="!data.enabled" class="text-sm text-warn">
        Static addresses are off: this deployment sells and allocates none (ONNET_FLEET_MAX_ADDRESSES is 0, or its
        address settings are incomplete: the boot log says which). The held ones are still released while the worker's
        address settings read.
      </p>
    </div>

    <section class="space-y-4" aria-labelledby="addresses-list">
      <h2 id="addresses-list" class="text-xl font-semibold text-ink">Addresses</h2>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Static addresses</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">Name</th>
              <th scope="col" class="px-4 py-3">Owner</th>
              <th scope="col" class="px-4 py-3">IP</th>
              <th scope="col" class="px-4 py-3">State</th>
              <th scope="col" class="px-4 py-3">Paid by</th>
              <th scope="col" class="px-4 py-3">Server</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="data === null">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="data.addresses.length === 0">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">No static address yet.</td>
            </tr>
            <template v-for="a in data?.addresses ?? []" :key="a.id">
              <tr class="address-row align-top hover:bg-hover">
                <th scope="row" class="px-4 py-3 text-left font-medium">
                  <span class="font-mono">{{ a.name }}</span>
                  <span class="block break-all font-mono text-xs font-normal text-ink-muted">{{ a.hostname }}</span>
                </th>
                <td class="whitespace-nowrap px-4 py-3">
                  <RouterLink v-if="a.owner" :to="`/admin/users/${a.owner.id}`" class="text-accent hover:text-ink-bright">
                    {{ a.owner.username }}
                  </RouterLink>
                  <span v-else class="text-ink-muted">Deleted account</span>
                </td>
                <td class="whitespace-nowrap px-4 py-3 font-mono text-xs">
                  {{ addressEndpoint(a) || 'None yet' }}
                  <span v-if="a.allocation_id" class="block text-ink-muted">{{ a.allocation_id }}</span>
                </td>
                <td class="px-4 py-3">
                  <span :class="addressStateClass(a.state)">{{ addressStateLabel(a) }}</span>
                  <span v-if="lapseCauseLabel(a)" class="block text-xs text-ink-muted">{{ lapseCauseLabel(a) }}</span>
                  <span v-if="a.dns_state === 'pending'" class="block text-xs text-ink-muted">Its name's record is being removed</span>
                  <span v-if="a.state === 'failed' && a.reason" class="block max-w-xs break-words text-xs text-danger">{{ a.reason }}</span>
                  <span v-if="a.state === 'failed' && a.retry_at" class="block text-xs text-ink-muted">
                    Tried again by itself <RelativeTime :value="a.retry_at" :now="now" />
                  </span>
                </td>
                <td class="px-4 py-3">
                  <template v-if="a.source === 'comp'">Admin grant</template>
                  <template v-else>
                    Subscription
                    <span v-if="a.stripe_subscription_id" class="block break-all font-mono text-xs text-ink-muted">{{ a.stripe_subscription_id }}</span>
                  </template>
                </td>
                <td class="px-4 py-3">
                  <RouterLink v-if="a.server" :to="`/admin/fleet/${a.server.id}`" class="break-words text-accent hover:text-ink-bright">
                    {{ a.server.game_name || `Server ${a.server.id}` }}
                  </RouterLink>
                  <span v-else class="text-ink-muted">None</span>
                </td>
                <td class="space-x-3 whitespace-nowrap px-4 py-3 text-right">
                  <button
                    v-if="a.state === 'failed'"
                    type="button"
                    :disabled="busy || !data?.enabled"
                    class="text-accent hover:text-ink-bright disabled:opacity-40"
                    @click="retry(a)"
                  >
                    Retry<span class="sr-only"> address {{ a.name }}</span>
                  </button>
                  <button type="button" :disabled="busy" class="text-ink hover:text-ink-bright disabled:opacity-40" @click="beginRename(a)">
                    Rename<span class="sr-only"> address {{ a.name }}</span>
                  </button>
                  <button
                    v-if="RELEASABLE.has(a.state)"
                    type="button"
                    :disabled="busy"
                    class="text-danger/80 hover:text-danger disabled:opacity-40"
                    @click="beginRelease(a)"
                  >
                    Release…<span class="sr-only"> address {{ a.name }}</span>
                  </button>
                </td>
              </tr>
              <tr v-if="renaming === a.id">
                <td colspan="7" class="px-4 py-3">
                  <form class="space-y-2" novalidate @submit.prevent="saveRename(a)">
                    <label :for="`address-rename-${a.id}`" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">
                      New name for {{ a.name }}
                    </label>
                    <div class="flex flex-wrap items-center gap-2">
                      <input
                        :id="`address-rename-${a.id}`"
                        v-model="newName"
                        type="text"
                        autocomplete="off"
                        autocapitalize="none"
                        spellcheck="false"
                        maxlength="32"
                        :aria-describedby="`address-rename-${a.id}-hint`"
                        :aria-invalid="renameProblem ? 'true' : undefined"
                        :class="fieldClass"
                      />
                      <button type="submit" :disabled="busy || newName.trim() === a.name" :class="primaryButton">Save name</button>
                      <button type="button" :disabled="busy" :class="secondaryButton" @click="renaming = null">Cancel</button>
                    </div>
                    <p :id="`address-rename-${a.id}-hint`" class="text-xs" :class="renameProblem ? 'text-danger' : 'text-ink-muted'">
                      {{ renameProblem || `${ADDRESS_NAME_RULE} A released address's old name is free again once renamed.` }}
                    </p>
                  </form>
                </td>
              </tr>
              <tr v-if="releasing === a.id">
                <td colspan="7" class="px-4 py-3">
                  <form class="space-y-3" @submit.prevent="confirmRelease(a)">
                    <p class="text-sm font-semibold text-danger">
                      Release {{ a.hostname }}? A server running at it moves to another address, its name's record goes,
                      and the cloud gives the address back. A subscription that pays for it is canceled in Stripe, not
                      here.
                    </p>
                    <label :for="`address-release-${a.id}`" class="block text-sm text-ink">
                      Type <span class="font-mono font-semibold">{{ a.name }}</span> to confirm
                    </label>
                    <input
                      :id="`address-release-${a.id}`"
                      v-model="typedName"
                      type="text"
                      autocomplete="off"
                      autocapitalize="none"
                      spellcheck="false"
                      class="w-full max-w-xs rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink focus:border-danger focus:outline-none"
                    />
                    <div class="flex flex-wrap gap-2">
                      <button
                        type="submit"
                        :disabled="busy || typedName.trim() !== a.name"
                        class="rounded-control bg-danger px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-danger/90 disabled:opacity-40"
                      >
                        {{ busy ? 'Releasing…' : 'Release now' }}
                      </button>
                      <button type="button" :disabled="busy" :class="secondaryButton" @click="releasing = null">Cancel</button>
                    </div>
                  </form>
                </td>
              </tr>
            </template>
          </tbody>
        </table>
      </div>
    </section>

    <!-- What fleet.sweep found the cloud holding that no row names. -->
    <section
      v-if="data && data.orphans.length > 0"
      class="space-y-3 rounded-panel border border-warn/40 bg-warn/10 p-4 sm:p-6"
      aria-labelledby="addresses-orphans"
    >
      <h2 id="addresses-orphans" class="text-xl font-semibold text-warn">Addresses no row names</h2>
      <p class="text-sm text-ink">
        The cloud holds these addresses for this deployment but no address row names them; release them in the AWS
        console after checking.
      </p>
      <ul class="divide-y divide-border rounded-panel border border-border bg-panel">
        <li v-for="o in data.orphans" :key="o.allocation_id" class="flex flex-wrap items-center justify-between gap-2 px-4 py-2 text-sm">
          <span class="font-mono text-xs text-ink">
            {{ o.allocation_id }}<template v-if="o.public_ip">, {{ o.public_ip }}</template>
          </span>
          <span class="text-xs text-ink-muted">
            <template v-if="o.address_id !== null">Tagged as address {{ o.address_id }}, </template>seen
            <RelativeTime v-if="o.seen_at" :value="o.seen_at" :now="now" />
          </span>
        </li>
      </ul>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/addresses: every static address (GET /api/admin/addresses), how many
// the cloud holds of what the deployment sells, each with its owner (to their
// page), name and host name, IP, state, what pays for it and the server
// running at it (to its fleet page); a retry of one the cloud refused (POST
// .../retry), a rename (PUT /api/admin/addresses/<id>) and a release behind the
// address's name typed (POST .../release); and the addresses the cloud holds
// that no row names, which the sweep never releases.
// The page polls, faster while an address is being set up or released.
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import { fetchAdminAddresses, releaseAddress, renameAddress, retryAddress } from '../../api/adminAddresses';
import { usePolling } from '../../composables/usePolling';
import type { AdminAddress, AdminAddressesResponse } from '../../types/addresses';
import {
  ADDRESS_NAME_RULE,
  addressEndpoint,
  addressNameProblem,
  addressStateClass,
  addressStateLabel,
  isAddressMoving,
  lapseCauseLabel,
} from '../../utils/addressView';

const MOVING_POLL_MS = 5_000;
const IDLE_POLL_MS = 30_000;
// The states an admin may release from (the rest are on their way out).
const RELEASABLE: ReadonlySet<string> = new Set(['allocating', 'ready', 'failed']);

const fieldClass =
  'w-48 rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink focus:border-accent focus:outline-none aria-[invalid=true]:border-danger';
const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-50';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const data = ref<AdminAddressesResponse | null>(null);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
// The address whose rename (or release) form is open, and what was typed.
const renaming = ref<number | null>(null);
const newName = ref('');
const renameTried = ref(false);
const releasing = ref<number | null>(null);
const typedName = ref('');
// The error banner shows a failed read (not an action's refusal).
let errorFromLoad = false;

const renameProblem = computed(() => (renameTried.value ? addressNameProblem(newName.value.trim()) : ''));

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
  errorFromLoad = false;
}

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchAdminAddresses(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      data.value = result.value;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (errorFromLoad) {
        error.value = '';
        errorFromLoad = false;
      }
      return true;
    }
    if (background && data.value !== null) {
      pollTrouble.value = true;
    } else {
      fail(result.error.message);
      errorFromLoad = true;
    }
    return false;
  },
  next: () => (data.value?.addresses.some((a) => isAddressMoving(a.state)) ? MOVING_POLL_MS : IDLE_POLL_MS),
});

function beginRename(a: AdminAddress) {
  releasing.value = null;
  renaming.value = a.id;
  newName.value = a.name;
  renameTried.value = false;
}

function beginRelease(a: AdminAddress) {
  renaming.value = null;
  releasing.value = a.id;
  typedName.value = '';
}

async function saveRename(a: AdminAddress) {
  if (busy.value) return;
  renameTried.value = true;
  const name = newName.value.trim();
  if (addressNameProblem(name)) return;
  busy.value = true;
  const result = await renameAddress(a.id, name);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  renaming.value = null;
  flash(`Renamed ${a.name} to ${result.value.name}.`);
  void poll.readAgain();
}

async function retry(a: AdminAddress) {
  if (busy.value) return;
  busy.value = true;
  const result = await retryAddress(a.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  flash(`${a.hostname} is being set up again.`);
  void poll.readAgain();
}

async function confirmRelease(a: AdminAddress) {
  if (busy.value || typedName.value.trim() !== a.name) return;
  busy.value = true;
  const result = await releaseAddress(a.id, a.name);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  releasing.value = null;
  flash(`${a.hostname} is being released: a server running at it moves to another address first.`);
  void poll.readAgain();
}
</script>
