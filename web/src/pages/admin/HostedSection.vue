<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Hosted servers</h1>
        <p class="text-sm text-ink-muted">
          The servers NovaWorld runs for approved accounts, each on a machine of its own. Approve accounts on
          the Users page.
        </p>
      </div>
      <div class="flex items-center gap-3">
        <p v-if="loadedAt" class="text-xs text-ink-muted">
          Updated <time :datetime="new Date(loadedAt).toISOString()">{{ new Date(loadedAt).toLocaleTimeString() }}</time>
        </p>
        <button
          type="button"
          class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40"
          :disabled="loading"
          @click="load(false)"
        >
          {{ loading ? 'Refreshing…' : 'Refresh' }}
        </button>
      </div>
    </header>

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them; the boxes below are their visible copies. -->
    <div role="status" class="sr-only">{{ message }}</div>
    <div role="alert" class="sr-only">{{ error }}</div>
    <div v-if="message" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">
      {{ message }}
    </div>
    <div v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      {{ error }}
    </div>
    <p v-if="pollTrouble" class="text-xs text-warn">
      The last refresh did not get through; this page keeps trying.
    </p>

    <dl v-if="fleet" class="grid gap-4 sm:grid-cols-3">
      <div class="rounded-panel border border-border bg-panel p-4">
        <dt class="text-xs uppercase tracking-wider text-ink-muted">Hosting</dt>
        <dd class="mt-1 text-lg font-semibold" :class="fleet.enabled ? 'text-online' : 'text-ink-muted'">
          {{ fleet.enabled ? 'On' : 'Off' }}
        </dd>
      </div>
      <div class="rounded-panel border border-border bg-panel p-4">
        <dt class="text-xs uppercase tracking-wider text-ink-muted">Provider</dt>
        <dd class="mt-1 break-all font-mono text-lg text-ink">{{ fleet.provider }}</dd>
      </div>
      <div class="rounded-panel border border-border bg-panel p-4">
        <dt class="text-xs uppercase tracking-wider text-ink-muted">Live</dt>
        <dd class="mt-1 text-lg font-semibold text-ink">{{ fleet.live }} of {{ fleet.cap }}</dd>
      </div>
    </dl>

    <section class="space-y-4" aria-labelledby="fleet-heading">
      <h2 id="fleet-heading" class="text-xl font-semibold text-ink">Servers</h2>
      <!-- relative: the rows' sr-only spans are absolutely positioned, and without a
           positioned ancestor they escape this scroller and widen the page on a phone. -->
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Hosted servers, live ones first, then the newest ended ones</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">ID</th>
              <th scope="col" class="px-4 py-3">Server</th>
              <th scope="col" class="px-4 py-3">Owner</th>
              <th scope="col" class="px-4 py-3">State</th>
              <th scope="col" class="px-4 py-3">Since</th>
              <th scope="col" class="px-4 py-3">Listing</th>
              <th scope="col" class="px-4 py-3">Machine</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="fleet === null">
              <td colspan="8" class="px-4 py-6 text-center text-ink-muted">{{ loading ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="fleet.servers.length === 0">
              <td colspan="8" class="px-4 py-6 text-center text-ink-muted">No hosted servers yet.</td>
            </tr>
            <tr v-for="s in fleet?.servers ?? []" :key="s.id" class="align-top hover:bg-hover">
              <td class="px-4 py-3 font-mono text-xs">{{ s.id }}</td>
              <th scope="row" class="px-4 py-3 text-left font-medium">
                <span class="break-words">{{ s.settings.game_name || 'Unnamed' }}</span>
                <span class="block text-xs font-normal text-ink-muted">
                  {{ s.settings.expansion === 'jox01' ? 'Escalation' : 'Joint Operations' }}
                  · {{ s.settings.max_players }} players
                </span>
              </th>
              <td class="px-4 py-3">
                <template v-if="s.owner">{{ s.owner.username }}</template>
                <span v-else class="text-ink-muted">(deleted)</span>
                <span v-if="s.service_account" class="block font-mono text-xs text-ink-muted">{{ s.service_account.username }}</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <span :class="stateBadgeClass(s.state)">{{ stateLabel(s.state) }}</span>
                <span v-if="s.restarts > 0" class="block text-xs text-warn">
                  {{ s.restarts }} restart{{ s.restarts === 1 ? '' : 's' }}
                </span>
                <span v-if="s.state === 'failed' && s.failure_reason" class="block max-w-xs whitespace-normal text-xs text-danger">
                  {{ s.failure_reason }}
                </span>
              </td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <time :datetime="isoOf(sinceOf(s))" :title="localOf(sinceOf(s))">{{ durationSince(sinceOf(s), now) }}</time>
                <span class="block">{{ sinceLabel(s) }}</span>
              </td>
              <td class="px-4 py-3">
                <template v-if="s.listing">
                  <span class="block break-words">{{ s.listing.server_name }}</span>
                  <span class="block font-mono text-xs text-ink-muted">{{ s.listing.host_ip }}:{{ s.listing.host_port }}</span>
                  <span class="block text-xs text-ink-muted">
                    {{ s.listing.players }}/{{ s.listing.max_players }} players
                    <template v-if="s.listing.mission_name"> · {{ s.listing.mission_name }}</template>
                  </span>
                </template>
                <span v-else class="text-ink-muted">Not listed</span>
              </td>
              <td class="px-4 py-3 text-xs">
                <span class="block font-mono">{{ s.provider || '(none)' }}</span>
                <span v-if="s.machine_id" class="block break-all font-mono text-ink-muted">{{ s.machine_id }}</span>
                <span v-if="s.machine_address" class="block break-all font-mono text-ink-muted">
                  {{ s.machine_address }}<template v-if="s.admin_port"> (admin {{ s.admin_port }})</template>
                </span>
              </td>
              <td class="space-x-3 whitespace-nowrap px-4 py-3 text-right">
                <button
                  v-if="s.state === 'running'"
                  type="button"
                  class="text-accent hover:text-ink-bright"
                  :aria-expanded="consoleOpen(s.id) ? 'true' : 'false'"
                  :aria-controls="consoleOpen(s.id) ? consolePanelId(s.id) : undefined"
                  @click="toggleConsole(s.id)"
                >
                  {{ consoleOpen(s.id) ? 'Close console' : 'Console' }}<span class="sr-only"> of server {{ s.id }}, {{ s.settings.game_name }}</span>
                </button>
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

    <!-- The consoles opened from the list: read-only, plus cycle. -->
    <section
      v-for="s in openConsoles"
      :id="consolePanelId(s.id)"
      :key="s.id"
      class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6"
      :aria-labelledby="`${consolePanelId(s.id)}-heading`"
    >
      <div class="flex flex-col gap-2 sm:flex-row sm:items-start sm:justify-between">
        <div class="min-w-0">
          <p class="text-xs uppercase tracking-[0.3em] text-ink-muted">Server #{{ s.id }}</p>
          <h2 :id="`${consolePanelId(s.id)}-heading`" class="mt-1 break-words text-xl font-semibold text-ink">
            {{ s.settings.game_name || 'Unnamed' }}
          </h2>
          <p class="text-sm text-ink-muted">
            <template v-if="s.owner">{{ s.owner.username }}'s hosted server</template>
            <template v-else>A hosted server whose owner was deleted</template>
          </p>
        </div>
        <button
          type="button"
          class="shrink-0 rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover"
          @click="toggleConsole(s.id)"
        >
          Close<span class="sr-only"> the console of server {{ s.id }}</span>
        </button>
      </div>
      <HostedServerConsole
        :target="{ kind: 'admin', id: s.id }"
        mode="admin"
        :server-name="s.listing?.server_name || s.settings.game_name"
        :heading-level="3"
        @gone="load(true)"
      />
    </section>

    <!-- The kill switch. -->
    <section class="space-y-4 rounded-panel border border-danger/40 p-4 sm:p-6" aria-labelledby="stop-all-heading">
      <h2 id="stop-all-heading" class="text-xl font-semibold text-danger">Stop all</h2>
      <p class="text-sm text-ink-muted">
        The kill switch: every live hosted server goes to stopping at once. Their matches end for everyone on
        them and their machines shut down. Owners can start them again while hosting stays on.
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
          Stop all {{ liveCount }} live hosted server{{ liveCount === 1 ? '' : 's' }}? Every match on them ends at
          once.
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
          <button
            type="button"
            :disabled="busy"
            class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:opacity-40"
            @click="cancelStopAll"
          >
            Cancel
          </button>
        </div>
      </form>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/hosted: the hosted servers' fleet (GET /api/admin/hosted), a stop per
// server (POST /api/admin/hosted/<id>/stop) and the kill switch (POST
// /api/admin/hosted/stop-all) behind a typed confirmation. The list refreshes
// itself, faster while a server is changing state. A running server's console
// opens below the list (HostedServerConsole, read-only plus cycle, on
// /api/admin/hosted/<id>/...); every open console makes its calls through this
// page's one console queue, since the service runs one console request per
// admin account at once (the Bearer token counting as one account).
import { computed, nextTick, onMounted, onUnmounted, ref } from 'vue';
import { fetchHostedFleet, stopAllFleetServers, stopFleetServer } from '../../api/admin';
import HostedServerConsole from '../../components/HostedServerConsole.vue';
import { provideConsoleQueue } from '../../composables/useConsoleQueue';
import type { FleetResponse, FleetServer } from '../../types/hostedServer';
import { isTransitioning, stateBadgeClass, stateLabel } from '../../utils/hostedState';
import { durationSince, isoOf, localOf } from '../../utils/utcTime';

const STOP_ALL_PHRASE = 'STOP ALL';
const TRANSITION_POLL_MS = 5_000;
const IDLE_POLL_MS = 30_000;
const RETRY_POLL_FIRST_MS = 5_000;
const RETRY_POLL_MAX_MS = 60_000;

const fleet = ref<FleetResponse | null>(null);
const loading = ref(false);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref<string | null>(null);
const error = ref<string | null>(null);
const confirmingAll = ref(false);
const typedPhrase = ref('');
const stopAllButton = ref<HTMLButtonElement | null>(null);
const stopAllWarning = ref<HTMLElement | null>(null);

const pollTrouble = ref(false);

let pollTimer: ReturnType<typeof setTimeout> | undefined;
let inFlight: AbortController | null = null;
let unmounted = false;
let retryDelay = RETRY_POLL_FIRST_MS;
// The error banner shows a failed read (not a stop's refusal).
let errorFromLoad = false;

const liveCount = computed(() => fleet.value?.servers.filter((s) => s.live).length ?? 0);

provideConsoleQueue();

// The servers whose console is open, in the order they were opened. A console
// shows while its server runs; one that has left running closes.
const openIds = ref<number[]>([]);

const openConsoles = computed(() => {
  const running = new Map((fleet.value?.servers ?? []).filter((s) => s.state === 'running').map((s) => [s.id, s]));
  return openIds.value.flatMap((id) => {
    const s = running.get(id);
    return s ? [s] : [];
  });
});

function consoleOpen(id: number): boolean {
  return openIds.value.includes(id);
}

function consolePanelId(id: number): string {
  return `hosted-console-panel-${id}`;
}

async function toggleConsole(id: number) {
  if (consoleOpen(id)) {
    openIds.value = openIds.value.filter((open) => open !== id);
    return;
  }
  openIds.value = [...openIds.value, id];
  await nextTick();
  document.getElementById(consolePanelId(id))?.scrollIntoView({ behavior: 'smooth', block: 'start' });
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
  if (!s.live && s.ended_at) return 'ago';
  return 'in this state';
}

function flash(text: string) {
  message.value = text;
  error.value = null;
}

function fail(text: string) {
  error.value = text;
  message.value = null;
  errorFromLoad = false;
}

// The next poll: faster while a server changes state, and after a failed poll
// from 5 s, doubling to a minute. A hidden tab skips its polls and reads the
// fleet again when it is shown (onVisibility).
function schedule(failed: boolean) {
  if (pollTimer) clearTimeout(pollTimer);
  pollTimer = undefined;
  if (unmounted) return;
  let delay: number;
  if (failed) {
    delay = retryDelay;
    retryDelay = Math.min(retryDelay * 2, RETRY_POLL_MAX_MS);
  } else {
    retryDelay = RETRY_POLL_FIRST_MS;
    const moving = fleet.value?.servers.some((s) => isTransitioning(s.state)) ?? false;
    delay = moving ? TRANSITION_POLL_MS : IDLE_POLL_MS;
  }
  pollTimer = setTimeout(() => {
    pollTimer = undefined;
    if (typeof document !== 'undefined' && document.hidden) return;
    void load(true);
  }, delay);
}

function onVisibility() {
  if (!document.hidden && !pollTimer) void load(true);
}

// `background` keeps the list (and only notes the trouble) on a failed poll.
async function load(background: boolean) {
  if (inFlight) {
    if (background) return;
    inFlight.abort();
  }
  const controller = new AbortController();
  inFlight = controller;
  if (!background) loading.value = true;
  const result = await fetchHostedFleet(controller.signal);
  if (inFlight !== controller) return;
  inFlight = null;
  loading.value = false;
  now.value = Date.now();
  if (result.ok) {
    fleet.value = result.value;
    loadedAt.value = now.value;
    pollTrouble.value = false;
    // A console whose server has left running closes for good.
    const running = new Set(result.value.servers.filter((s) => s.state === 'running').map((s) => s.id));
    if (openIds.value.some((id) => !running.has(id))) {
      openIds.value = openIds.value.filter((id) => running.has(id));
    }
    // A failed read's banner goes with the next good one (a stop's stays).
    if (errorFromLoad) {
      error.value = null;
      errorFromLoad = false;
    }
    schedule(false);
    return;
  }
  if (background && fleet.value !== null) {
    pollTrouble.value = true;
  } else {
    fail(result.error.message);
    errorFromLoad = true;
  }
  schedule(true);
}

async function stopOne(s: FleetServer) {
  if (busy.value) return;
  const who = s.owner ? `${s.owner.username}'s` : 'the';
  const players = s.listing ? ` The match ends for the ${s.listing.players} player(s) on it.` : '';
  if (!window.confirm(`Stop ${who} hosted server #${s.id} (${s.settings.game_name})?${players} Its machine shuts down.`)) return;
  busy.value = true;
  const result = await stopFleetServer(s.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
  } else {
    flash(`Server #${s.id} is stopping.`);
  }
  await load(false);
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
  await load(false);
}

onMounted(() => {
  void load(false);
  document.addEventListener('visibilitychange', onVisibility);
});

onUnmounted(() => {
  unmounted = true;
  if (pollTimer) clearTimeout(pollTimer);
  document.removeEventListener('visibilitychange', onVisibility);
  inFlight?.abort();
  inFlight = null;
});
</script>
