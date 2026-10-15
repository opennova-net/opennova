<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Fleet</h1>
        <p class="text-sm text-ink-muted">
          The machines NovaWorld runs hosted servers on, and the servers on them. A shared machine holds several
          servers, an own machine exactly one.
        </p>
      </div>
      <div class="flex items-center gap-3">
        <p v-if="loadedAt" class="text-xs text-ink-muted">Updated <RelativeTime :value="loadedAt" :now="now" /></p>
        <button type="button" :class="secondaryButton" :disabled="poll.loading.value" @click="poll.refresh">
          {{ poll.loading.value ? 'Refreshing…' : 'Refresh' }}
        </button>
      </div>
    </header>

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them; the boxes below are their visible copies. -->
    <div role="status" class="sr-only">{{ message }}</div>
    <div role="alert" class="sr-only">{{ error }}</div>
    <div v-if="message" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">{{ message }}</div>
    <div v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">{{ error }}</div>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>

    <dl v-if="fleet" class="grid grid-cols-1 gap-3 min-[420px]:grid-cols-3">
      <StatTile label="Hosting" :value="fleet.enabled ? 'On' : 'Off'" :detail="fleet.enabled ? 'Owners can start servers' : 'No server can start'" />
      <StatTile label="Live servers" :value="`${fleet.live} of ${fleet.cap}`" detail="The service's cap on live servers" />
      <StatTile label="Machines" :value="String(activeMachines.length)" :detail="machineBreakdown" />
    </dl>

    <!-- Machines. -->
    <section class="space-y-4" aria-labelledby="fleet-machines">
      <div class="flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
        <h2 id="fleet-machines" class="text-xl font-semibold text-ink">Machines</h2>
        <button type="button" :class="secondaryButton" :disabled="busy || !fleet" aria-describedby="fleet-roll-hint" @click="roll">
          Roll the fleet…
        </button>
      </div>
      <p id="fleet-roll-hint" class="text-xs text-ink-muted">
        Rolling drains every shared machine and moves its servers to fresh ones (a new image, say); the moves count
        as no restart.
      </p>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Machines</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">Machine</th>
              <th scope="col" class="px-4 py-3">Kind</th>
              <th scope="col" class="px-4 py-3">State</th>
              <th scope="col" class="px-4 py-3">Instance type</th>
              <th scope="col" class="px-4 py-3">Servers</th>
              <th scope="col" class="px-4 py-3">Address</th>
              <th scope="col" class="px-4 py-3">Data sent this month</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="fleet === null">
              <td colspan="8" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="fleet.machines.length === 0">
              <td colspan="8" class="px-4 py-6 text-center text-ink-muted">No machine is running.</td>
            </tr>
            <tr v-for="m in fleet?.machines ?? []" :key="m.id" class="fleet-machine-row align-top hover:bg-hover">
              <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-mono text-xs font-normal">{{ m.id }}</th>
              <td class="whitespace-nowrap px-4 py-3">{{ machineKindLabel(m.kind) }}</td>
              <td class="whitespace-nowrap px-4 py-3" :class="m.state === 'failed' || m.state === 'lost' ? 'text-danger' : m.state === 'ready' ? 'text-online' : 'text-warn'">
                {{ machineStateLabel(m.state) }}
              </td>
              <td class="whitespace-nowrap px-4 py-3 font-mono text-xs">{{ m.instance_type || 'Unknown' }}</td>
              <td class="whitespace-nowrap px-4 py-3">{{ m.slots_used }} of {{ m.slots }}</td>
              <td class="whitespace-nowrap px-4 py-3 font-mono text-xs">{{ m.public_address || 'None yet' }}</td>
              <td class="whitespace-nowrap px-4 py-3">{{ formatBytes(m.egress_bytes_month) }}</td>
              <td class="space-x-3 whitespace-nowrap px-4 py-3 text-right">
                <button
                  v-if="m.state === 'ready' || m.state === 'booting'"
                  type="button"
                  :disabled="busy"
                  class="text-warn/90 hover:text-warn disabled:opacity-40"
                  @click="onMachine(m, 'drain')"
                >
                  Drain<span class="sr-only"> machine {{ m.id }}</span>
                </button>
                <button
                  v-if="m.state !== 'ended' && m.state !== 'ending'"
                  type="button"
                  :disabled="busy"
                  class="text-danger/80 hover:text-danger disabled:opacity-40"
                  @click="onMachine(m, 'end')"
                >
                  End<span class="sr-only"> machine {{ m.id }}</span>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- Servers. -->
    <section class="space-y-4" aria-labelledby="fleet-servers">
      <h2 id="fleet-servers" class="text-xl font-semibold text-ink">Servers</h2>
      <form class="flex flex-wrap items-end gap-3" @submit.prevent="applyFilter">
        <label class="space-y-1">
          <span class="block text-xs font-medium text-ink-muted">Owner</span>
          <input
            id="fleet-filter-owner"
            v-model="ownerInput"
            type="search"
            autocomplete="off"
            placeholder="Any owner"
            class="rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          />
        </label>
        <label class="space-y-1">
          <span class="block text-xs font-medium text-ink-muted">Servers</span>
          <select
            id="fleet-filter-state"
            v-model="stateInput"
            class="rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          >
            <option value="all">Live, then the newest ended</option>
            <option value="live">Live only</option>
            <option value="ended">Ended only</option>
          </select>
        </label>
        <button type="submit" :class="secondaryButton">Apply</button>
      </form>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Hosted servers</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">Server</th>
              <th scope="col" class="px-4 py-3">Owner</th>
              <th scope="col" class="px-4 py-3">State</th>
              <th scope="col" class="px-4 py-3">Players</th>
              <th scope="col" class="px-4 py-3">Machine and slot</th>
              <th scope="col" class="px-4 py-3">Since</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="fleet === null">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="fleet.servers.length === 0">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">No hosted server matches.</td>
            </tr>
            <tr v-for="s in fleet?.servers ?? []" :key="s.id" class="fleet-server-row align-top hover:bg-hover">
              <th scope="row" class="px-4 py-3 text-left font-medium">
                <RouterLink :to="`/admin/fleet/${s.id}`" class="break-words text-accent hover:text-ink-bright">
                  {{ s.settings.game_name || 'Unnamed server' }}
                </RouterLink>
                <span class="block text-xs font-normal text-ink-muted">
                  Server {{ s.id }}, {{ s.game_display_name || s.settings.game }}<template v-if="s.own_machine">, own machine</template>
                </span>
              </th>
              <td class="whitespace-nowrap px-4 py-3">
                <template v-if="s.owner">
                  {{ s.owner.username }}
                  <span class="block text-xs text-ink-muted">{{ s.owner.tier ? `Tier ${s.owner.tier}` : 'No tier' }}</span>
                </template>
                <span v-else class="text-ink-muted">Deleted account</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <span :class="stateBadgeClass(s.state)">{{ stateLabel(s.state) }}</span>
                <span v-if="s.restarts > 0" class="block text-xs text-warn">{{ s.restarts }} restart{{ s.restarts === 1 ? '' : 's' }}</span>
                <span v-if="s.state === 'failed' && s.failure_reason" class="block max-w-xs whitespace-normal text-xs text-danger">{{ s.failure_reason }}</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <template v-if="s.players !== null">{{ s.players }} of {{ s.settings.max_players }}</template>
                <span v-else class="text-ink-muted">Not listed</span>
              </td>
              <td class="px-4 py-3 text-xs">
                <template v-if="s.machine_ref">
                  <span class="block whitespace-nowrap font-mono">{{ s.machine_ref }}</span>
                  <span class="block text-ink-muted">
                    Slot {{ s.slot ?? 'none' }}<template v-if="s.game_port">, game port {{ s.game_port }}</template>
                  </span>
                </template>
                <span v-else class="text-ink-muted">Not placed</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime :value="sinceOf(s)" :now="now" />
                <span class="block">{{ sinceLabel(s) }}</span>
              </td>
              <td class="space-x-3 whitespace-nowrap px-4 py-3 text-right">
                <RouterLink v-if="s.state === 'running'" :to="`/admin/fleet/${s.id}/console`" class="text-accent hover:text-ink-bright">
                  Console<span class="sr-only"> of server {{ s.id }}</span>
                </RouterLink>
                <button
                  v-if="s.live && s.state !== 'stopping'"
                  type="button"
                  :disabled="busy"
                  class="text-danger/80 hover:text-danger disabled:opacity-40"
                  @click="stopOne(s)"
                >
                  Stop<span class="sr-only"> server {{ s.id }}, {{ s.settings.game_name }}</span>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- The kill switch. -->
    <section class="space-y-4 rounded-panel border border-danger/40 p-4 sm:p-6" aria-labelledby="stop-all-heading">
      <h2 id="stop-all-heading" class="text-xl font-semibold text-danger">Stop all</h2>
      <p class="text-sm text-ink-muted">
        The kill switch: every live hosted server goes to stopping at once. Their matches end for everyone on them.
        Owners can start them again while hosting stays on.
      </p>
      <button
        v-if="!confirmingAll"
        ref="stopAllButton"
        type="button"
        :disabled="busy || liveCount === 0"
        class="rounded-control border border-danger px-4 py-2 text-sm font-medium text-danger transition hover:bg-danger/10 disabled:opacity-40"
        @click="beginStopAll"
      >
        Stop all live servers…
      </button>
      <form v-else class="space-y-3" @submit.prevent="confirmStopAll">
        <p ref="stopAllWarning" tabindex="-1" class="text-sm font-semibold text-danger focus:outline-none">
          Stop all {{ liveCount }} live hosted server{{ liveCount === 1 ? '' : 's' }}? Every match on them ends at once.
        </p>
        <label for="stop-all-confirm" class="block text-sm text-ink">
          Type <span class="font-mono font-semibold">{{ STOP_ALL_PHRASE }}</span> to confirm
        </label>
        <input
          id="stop-all-confirm"
          v-model="typedPhrase"
          type="text"
          autocomplete="off"
          autocapitalize="characters"
          spellcheck="false"
          class="w-full max-w-xs rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink focus:border-danger focus:outline-none"
        />
        <div class="flex flex-wrap gap-2">
          <button
            type="submit"
            :disabled="busy || typedPhrase.trim() !== STOP_ALL_PHRASE"
            class="rounded-control bg-danger px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-danger/90 disabled:opacity-40"
          >
            {{ busy ? 'Stopping…' : 'Stop all now' }}
          </button>
          <button type="button" :disabled="busy" :class="secondaryButton" @click="cancelStopAll">Cancel</button>
        </div>
      </form>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/fleet: the fleet (GET /api/admin/hosted?owner=&state=): its machines
// (kind, state, instance type, servers on it of its slots, address, data
// sent this month) with drain and end (POST /api/admin/fleet/machines/<id>/...)
// and the roll (POST /api/admin/fleet/roll); its servers (owner and tier,
// state, players, machine and slot), each to its own page
// (/admin/fleet/<id>), with a stop; and the kill switch (POST
// /api/admin/hosted/stop-all) behind a typed confirmation. The page polls,
// faster while a server changes state.
import { computed, nextTick, ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import StatTile from '../../components/charts/StatTile.vue';
import { fetchHostedFleet, machineAction, rollFleet, stopAllFleetServers, stopFleetServer } from '../../api/admin';
import { usePolling } from '../../composables/usePolling';
import type { FleetMachine, FleetQuery, FleetResponse, FleetServer } from '../../types/hostedServer';
import { machineKindLabel, machineStateLabel } from '../../utils/adminView';
import { formatBytes } from '../../utils/format';
import { isTransitioning, stateBadgeClass, stateLabel } from '../../utils/hostedState';

const STOP_ALL_PHRASE = 'STOP ALL';
const TRANSITION_POLL_MS = 5_000;
const IDLE_POLL_MS = 30_000;

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const fleet = ref<FleetResponse | null>(null);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
const confirmingAll = ref(false);
const typedPhrase = ref('');
const stopAllButton = ref<HTMLButtonElement | null>(null);
const stopAllWarning = ref<HTMLElement | null>(null);
const ownerInput = ref('');
const stateInput = ref<NonNullable<FleetQuery['state']>>('all');
const query = ref<FleetQuery>({});
// The error banner shows a failed read (not an action's refusal).
let errorFromLoad = false;

const liveCount = computed(() => fleet.value?.servers.filter((s) => s.live).length ?? 0);
const activeMachines = computed(() => (fleet.value?.machines ?? []).filter((m) => m.state !== 'ended'));
const machineBreakdown = computed(() => {
  const shared = activeMachines.value.filter((m) => m.kind === 'shared').length;
  const own = activeMachines.value.length - shared;
  return `${shared} shared, ${own} own`;
});

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
    const result = await fetchHostedFleet(query.value, signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      fleet.value = result.value;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (errorFromLoad) {
        error.value = '';
        errorFromLoad = false;
      }
      return true;
    }
    if (background && fleet.value !== null) {
      pollTrouble.value = true;
    } else {
      fail(result.error.message);
      errorFromLoad = true;
    }
    return false;
  },
  next: () => {
    const moving =
      (fleet.value?.servers.some((s) => isTransitioning(s.state)) ?? false) ||
      (fleet.value?.machines.some((m) => m.state !== 'ready' && m.state !== 'ended' && m.state !== 'failed') ?? false);
    return moving ? TRANSITION_POLL_MS : IDLE_POLL_MS;
  },
});

function applyFilter() {
  const owner = ownerInput.value.trim();
  query.value = { ...(owner ? { owner } : {}), state: stateInput.value };
  void poll.refresh();
}

// Since when a server has been in its state: running since it first ran, an
// ended one since it ended, any other since its last change.
function sinceOf(s: FleetServer): string {
  if (s.state === 'running' && s.running_at) return s.running_at;
  if (!s.live && s.ended_at) return s.ended_at;
  return s.updated_at;
}

function sinceLabel(s: FleetServer): string {
  if (s.state === 'running' && s.running_at) return 'running';
  if (!s.live && s.ended_at) return 'ended';
  return 'in this state';
}

async function stopOne(s: FleetServer) {
  if (busy.value) return;
  const who = s.owner ? `${s.owner.username}'s` : 'the';
  const players = s.players ? ` The match ends for the ${s.players} player${s.players === 1 ? '' : 's'} on it.` : '';
  if (!window.confirm(`Stop ${who} hosted server ${s.id} (${s.settings.game_name})?${players}`)) return;
  busy.value = true;
  const result = await stopFleetServer(s.id);
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash(`Server ${s.id} is stopping.`);
  await poll.readAgain();
}

async function onMachine(m: FleetMachine, action: 'drain' | 'end') {
  if (busy.value) return;
  const servers = m.slots_used;
  const prompt =
    action === 'drain'
      ? `Drain machine ${m.id}? It takes no new server; its ${servers} server${servers === 1 ? '' : 's'} move to another machine at their next start.`
      : `End machine ${m.id} now? ${servers === 0 ? 'No server runs on it.' : `Its ${servers} server${servers === 1 ? '' : 's'} stop, and their matches end for everyone on them.`}`;
  if (!window.confirm(prompt)) return;
  busy.value = true;
  const result = await machineAction(m.id, action);
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash(action === 'drain' ? `Machine ${m.id} is draining.` : `Machine ${m.id} is ending.`);
  await poll.readAgain();
}

async function roll() {
  if (busy.value) return;
  if (!window.confirm('Roll the fleet? Every shared machine drains and its servers move to fresh machines; each server is down while it moves.')) {
    return;
  }
  busy.value = true;
  const result = await rollFleet();
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash('The fleet is rolling: shared machines drain one by one.');
  await poll.readAgain();
}

async function beginStopAll() {
  confirmingAll.value = true;
  typedPhrase.value = '';
  await nextTick();
  stopAllWarning.value?.focus();
}

async function cancelStopAll() {
  confirmingAll.value = false;
  typedPhrase.value = '';
  await nextTick();
  stopAllButton.value?.focus();
}

async function confirmStopAll() {
  if (busy.value || typedPhrase.value.trim() !== STOP_ALL_PHRASE) return;
  busy.value = true;
  const result = await stopAllFleetServers();
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  const n = result.value;
  flash(n === 0 ? 'No server was live, so none was stopped.' : `${n} server${n === 1 ? ' is' : 's are'} stopping.`);
  await cancelStopAll();
  await poll.readAgain();
}
</script>
