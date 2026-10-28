<template>
  <div class="space-y-8">
    <PageHeader title="Billing" :tabs="BILLING_TABS" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template v-if="data" #meta>
        <span id="addresses-held">{{ data.held }} of {{ data.sellable }} held</span>
        <span class="font-mono">{{ data.zone }}</span>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" :poll-trouble="pollTrouble" />
    <Notice v-if="data && !data.enabled" tone="warn" class="addresses-off">Static addresses are off on this deployment.</Notice>

    <div class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">Static addresses</caption>
        <thead>
          <tr>
            <th scope="col">Name</th>
            <th scope="col">Owner</th>
            <th scope="col">IP</th>
            <th scope="col">State</th>
            <th scope="col">Paid by</th>
            <th scope="col">Server</th>
            <th v-if="showMachine" scope="col">Machine</th>
            <th scope="col"><span class="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          <TableState v-if="!data?.addresses.length" :colspan="columns" :rows="data?.addresses ?? null" :loading="poll.loading.value" empty="No static addresses yet." />
          <template v-for="a in data?.addresses ?? []" :key="a.id">
            <tr class="address-row">
              <th scope="row">
                <span class="font-mono">{{ a.name }}</span>
                <span class="block break-all font-mono text-xs font-normal text-ink-muted">{{ a.hostname }}</span>
              </th>
              <td class="whitespace-nowrap"><PlayerLink :id="a.owner?.id ?? null" :name="a.owner?.username ?? ''" /></td>
              <td class="whitespace-nowrap font-mono text-xs">
                {{ addressEndpoint(a) || 'None yet' }}
                <span v-if="a.allocation_id" class="block text-ink-muted">{{ a.allocation_id }}</span>
              </td>
              <td class="address-state">
                <span :class="addressStateClass(a.state)">{{ addressStateLabel(a) }}</span>
                <span v-if="lapseCauseLabel(a)" class="block text-xs text-ink-muted">{{ lapseCauseLabel(a) }}</span>
                <span v-if="a.dns_state === 'pending'" class="block text-xs text-ink-muted">Record being removed</span>
                <span v-if="a.state === 'failed' && a.reason" class="block max-w-xs break-words text-xs text-danger">{{ a.reason }}</span>
                <span v-if="a.state === 'failed' && a.retry_at" class="block text-xs text-ink-muted">Retries <RelativeTime :value="a.retry_at" :now="now" /></span>
              </td>
              <td>
                <template v-if="a.source === 'comp'">Admin grant</template>
                <template v-else>
                  Subscription
                  <span v-if="a.stripe_subscription_id" class="block break-all font-mono text-xs text-ink-muted">{{ a.stripe_subscription_id }}</span>
                </template>
              </td>
              <td>
                <ServerLink v-if="a.server" :id="a.server.id" :name="a.server.game_name" scope="admin" />
                <span v-else class="text-ink-muted">None</span>
              </td>
              <td v-if="showMachine">
                <MachineLink v-if="a.machine_id !== null" :id="a.machine_id" />
              </td>
              <td class="whitespace-nowrap text-right">
                <button v-if="a.state === 'failed'" type="button" class="btn-ghost btn-sm" :disabled="busy || !data?.enabled" @click="retry(a)">
                  Retry<span class="sr-only"> address {{ a.name }}</span>
                </button>
                <button type="button" class="btn-ghost btn-sm" :disabled="busy" @click="beginRename(a)">
                  Rename<span class="sr-only"> address {{ a.name }}</span>
                </button>
                <button v-if="RELEASABLE.has(a.state)" type="button" class="btn-ghost btn-sm text-danger" :disabled="busy" @click="beginRelease(a)">
                  Release…<span class="sr-only"> address {{ a.name }}</span>
                </button>
              </td>
            </tr>
            <tr v-if="renaming === a.id">
              <td :colspan="columns">
                <form class="flex flex-wrap items-center gap-2" novalidate @submit.prevent="saveRename(a)">
                  <label :for="`address-rename-${a.id}`" class="text-xs text-ink-muted">New name for {{ a.name }}</label>
                  <input
                    :id="`address-rename-${a.id}`"
                    v-model="newName"
                    type="text"
                    autocomplete="off"
                    autocapitalize="none"
                    spellcheck="false"
                    maxlength="32"
                    :title="ADDRESS_NAME_RULE"
                    :aria-describedby="`address-rename-${a.id}-hint`"
                    :aria-invalid="renameProblem ? 'true' : undefined"
                    class="field w-48 py-1 font-mono"
                  />
                  <button type="submit" class="btn-primary btn-sm" :disabled="busy || newName.trim() === a.name">Save</button>
                  <button type="button" class="btn-secondary btn-sm" :disabled="busy" @click="renaming = null">Cancel</button>
                  <p v-if="renameProblem" :id="`address-rename-${a.id}-hint`" class="basis-full text-xs text-danger">{{ renameProblem }}</p>
                </form>
              </td>
            </tr>
            <tr v-if="releasing === a.id">
              <td :colspan="columns">
                <form class="flex flex-wrap items-center gap-2" @submit.prevent="confirmRelease(a)">
                  <label :for="`address-release-${a.id}`" class="text-sm text-danger">
                    Release {{ a.hostname }}? Its server moves to another address. Type <span class="font-mono font-semibold">{{ a.name }}</span>:
                  </label>
                  <input
                    :id="`address-release-${a.id}`"
                    v-model="typedName"
                    type="text"
                    autocomplete="off"
                    autocapitalize="none"
                    spellcheck="false"
                    class="field w-40 py-1 font-mono"
                  />
                  <button type="submit" class="btn-danger btn-sm" :disabled="busy || typedName.trim() !== a.name">{{ busy ? 'Releasing…' : 'Release now' }}</button>
                  <button type="button" class="btn-secondary btn-sm" :disabled="busy" @click="releasing = null">Cancel</button>
                </form>
              </td>
            </tr>
          </template>
        </tbody>
      </table>
    </div>

    <section v-if="data && data.orphans.length > 0" class="space-y-3" aria-labelledby="addresses-orphans">
      <h2 id="addresses-orphans" class="section-heading">Held by the cloud, not by an address</h2>
      <div class="table-wrap">
        <table class="data-table">
          <thead>
            <tr>
              <th scope="col">Allocation</th>
              <th scope="col">IP</th>
              <th scope="col">Tagged as</th>
              <th scope="col">Seen</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="o in data.orphans" :key="o.allocation_id" class="address-orphan">
              <th scope="row" class="font-mono text-xs font-normal">{{ o.allocation_id }}</th>
              <td class="font-mono text-xs">{{ o.public_ip }}</td>
              <td>{{ o.address_id !== null ? `Address ${o.address_id}` : '' }}</td>
              <td class="whitespace-nowrap text-xs text-ink-muted"><RelativeTime v-if="o.seen_at" :value="o.seen_at" :now="now" /></td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/billing/addresses: every static address, its owner, IP, state, what
// pays for it and its server, with a retry, a rename and a release behind its
// name typed; then what the cloud holds that no address names. Polled, faster
// while one moves.
import { computed, ref } from 'vue';
import RelativeTime from '../../../components/RelativeTime.vue';
import MachineLink from '../../../components/links/MachineLink.vue';
import PlayerLink from '../../../components/links/PlayerLink.vue';
import ServerLink from '../../../components/links/ServerLink.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import Notice from '../../../components/ui/Notice.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TableState from '../../../components/ui/TableState.vue';
import { fetchAdminAddresses, releaseAddress, renameAddress, retryAddress } from '../../../api/adminAddresses';
import { usePolling } from '../../../composables/usePolling';
import type { AdminAddress, AdminAddressesResponse } from '../../../types/addresses';
import {
  ADDRESS_NAME_RULE,
  addressEndpoint,
  addressNameProblem,
  addressStateClass,
  addressStateLabel,
  isAddressMoving,
  lapseCauseLabel,
} from '../../../utils/addressView';
import { BILLING_TABS } from '../../../utils/adminNav';

const MOVING_POLL_MS = 5_000;
const IDLE_POLL_MS = 30_000;
// The states an admin may release from (the rest are on their way out).
const RELEASABLE: ReadonlySet<string> = new Set(['allocating', 'ready', 'failed']);

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
// The error box shows a failed read (not an action's refusal).
let errorFromLoad = false;

// The Machine column, while any address names one.
const showMachine = computed(() => (data.value?.addresses ?? []).some((a) => a.machine_id !== null));
const columns = computed(() => (showMachine.value ? 8 : 7));
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
  flash(`${a.hostname} is being released.`);
  void poll.readAgain();
}
</script>
