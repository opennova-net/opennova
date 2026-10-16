<template>
  <div class="space-y-8">
    <!-- Live regions, always in the tab so a screen reader hears what lands
         in them; the boxes below are their visible copies. -->
    <p role="status" class="sr-only">{{ status }}</p>
    <p role="alert" class="sr-only">{{ alertText }}</p>
    <p v-if="status" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-3 text-sm text-online">
      {{ status }}
    </p>
    <p v-if="alertText" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">
      {{ alertText }}
    </p>

    <!-- Where the server is in its life. -->
    <section class="space-y-3" :aria-labelledby="ids.progress">
      <h2 :id="ids.progress" tabindex="-1" class="text-lg font-semibold text-ink focus:outline-none">State</h2>
      <ol v-if="stepIndex >= 0" class="flex flex-col gap-2 sm:flex-row sm:gap-1">
        <li
          v-for="(step, index) in OWNER_START_STEPS"
          :key="step.label"
          class="flex items-center gap-2 sm:flex-1 sm:flex-col sm:items-stretch"
          :aria-current="index === stepIndex ? 'step' : undefined"
        >
          <span aria-hidden="true" class="h-2.5 w-2.5 shrink-0 rounded-full sm:h-1.5 sm:w-full" :class="stepBarClass(index)"></span>
          <span class="text-xs" :class="index === stepIndex ? 'font-semibold text-ink' : 'text-ink-muted'">
            {{ step.label }}<span class="sr-only">{{ stepSuffix(index) }}</span>
          </span>
        </li>
      </ol>
      <p class="rounded-panel border p-3 text-sm" :class="stateNoteClass">
        <span class="font-semibold">{{ ownerStateLabel(server.state) }}.</span>
        {{ stateDescription(server.state) }}
      </p>
      <p v-if="server.state === 'failed' && server.failure_reason" class="break-words text-sm text-danger">
        {{ server.failure_reason }}
      </p>
      <p v-if="server.live && server.restarts > 0" class="text-sm text-warn">
        It crashed and was restarted {{ server.restarts === 1 ? 'once' : `${server.restarts} times` }}.
      </p>
      <p v-if="server.state === 'stopping'" class="text-sm text-ink-muted">{{ STOPPING_STILL_COUNTS }}</p>
      <dl class="grid grid-cols-[auto_1fr] gap-x-6 gap-y-1 text-sm">
        <dt class="text-ink-muted">Requested</dt>
        <dd class="text-ink"><RelativeTime :value="server.created_at" :now="now" /></dd>
        <template v-if="server.running_at">
          <dt class="text-ink-muted">First running</dt>
          <dd class="text-ink"><RelativeTime :value="server.running_at" :now="now" /></dd>
        </template>
        <template v-if="server.ended_at">
          <dt class="text-ink-muted">Ended</dt>
          <dd class="text-ink"><RelativeTime :value="server.ended_at" :now="now" /></dd>
        </template>
      </dl>
    </section>

    <!-- The data it runs on, and the update waiting when its data set has
         newer data than the trees it was started on. -->
    <section class="space-y-3" :aria-labelledby="ids.data">
      <h2 :id="ids.data" tabindex="-1" class="text-lg font-semibold text-ink focus:outline-none">Game data</h2>
      <dl class="grid grid-cols-[auto_1fr] gap-x-6 gap-y-1 text-sm">
        <dt class="text-ink-muted">{{ gameName }}</dt>
        <dd class="text-ink">{{ dataVersionText(server.data.base) }}</dd>
        <template v-if="server.expansion || server.data.expansion">
          <dt class="text-ink-muted">{{ expansionName }}</dt>
          <dd class="text-ink">{{ dataVersionText(server.data.expansion) }}</dd>
        </template>
      </dl>
      <p v-if="server.update_requested" class="rounded-panel border border-warn/40 bg-warn/10 p-3 text-sm text-warn">
        Update requested. The server restarts on the current data shortly; its players drop while it does.
      </p>
      <div
        v-else-if="server.update_available"
        id="hosted-server-update"
        class="space-y-3 rounded-panel border border-accent/40 bg-accent/10 p-3 text-sm text-ink"
      >
        <p>
          {{ updateText }}
          <template v-if="scope === 'owner'">Your server keeps the data it started on until you update it.</template>
          <template v-else>It keeps the data it started on until its owner updates it, or an admin updates every outdated server.</template>
        </p>
        <template v-if="scope === 'owner' && server.live && server.state !== 'stopping'">
          <button
            v-if="!confirmingUpdate"
            ref="updateButton"
            type="button"
            :disabled="busy"
            class="rounded-control bg-accent px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-accent/90 disabled:opacity-40"
            @click="beginUpdate"
          >
            Update…
          </button>
          <form v-else class="space-y-3" @submit.prevent="confirmUpdate">
            <p ref="updateWarning" tabindex="-1" class="text-sm font-semibold text-warn focus:outline-none">
              Updating restarts the server and drops its players.
            </p>
            <div class="flex flex-wrap gap-2">
              <button
                type="submit"
                :disabled="busy"
                class="rounded-control bg-accent px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-accent/90 disabled:opacity-40"
              >
                {{ busy ? 'Updating…' : 'Update now' }}
              </button>
              <button
                type="button"
                :disabled="busy"
                class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:opacity-40"
                @click="cancelUpdate"
              >
                Cancel
              </button>
            </div>
          </form>
        </template>
      </div>
    </section>

    <!-- The listing the server's NovaWorld account holds, with the owner's
         pushes to it that the console lacks, while it runs. -->
    <section v-if="server.live" class="space-y-3" :aria-labelledby="ids.listing">
      <h2 :id="ids.listing" class="text-lg font-semibold text-ink">Listing</h2>
      <div v-if="shownListing" class="space-y-3 rounded-panel border border-border p-4">
        <dl class="grid gap-x-6 gap-y-2 text-sm sm:grid-cols-[auto_1fr]">
          <dt class="text-ink-muted">Name</dt>
          <dd class="break-words text-ink">{{ shownListing.name }}</dd>
          <template v-if="shownListing.message">
            <dt class="text-ink-muted">Message</dt>
            <dd class="break-words text-ink">{{ shownListing.message }}</dd>
          </template>
          <dt class="text-ink-muted">Mission</dt>
          <dd class="break-words text-ink">{{ shownListing.mission || 'No mission yet' }}</dd>
          <dt class="text-ink-muted">Players</dt>
          <dd class="text-ink">{{ shownListing.players }} of {{ shownListing.maxPlayers }}</dd>
          <dt class="text-ink-muted">Address</dt>
          <dd class="break-all font-mono text-ink">{{ shownListing.address }}</dd>
        </dl>
        <p class="text-sm text-ink-muted">Players find it in the server browser.</p>
      </div>
      <p v-else-if="server.state === 'running'" class="text-sm text-warn">It is not in the server browser right now.</p>
      <p v-else class="text-sm text-ink-muted">It shows up in the server browser once it is ready.</p>
      <MyServerCard
        v-if="scope === 'owner' && listing && server.state === 'running'"
        :key="listing.rid"
        embedded
        :server="listing"
        :now="now"
        @changed="listingSoon"
        @session-ended="emit('session-ended')"
      />
    </section>

    <!-- Every transition, oldest first. -->
    <section v-if="events.length > 0" class="space-y-3" :aria-labelledby="ids.timeline">
      <h2 :id="ids.timeline" class="text-lg font-semibold text-ink">Timeline</h2>
      <details>
        <summary class="cursor-pointer text-sm text-accent hover:text-ink-bright">
          {{ events.length }} event{{ events.length === 1 ? '' : 's' }}, oldest first
        </summary>
        <ol class="mt-3 max-h-96 space-y-3 overflow-y-auto border-l border-border pl-4" tabindex="0" :aria-labelledby="ids.timeline">
          <li v-for="(event, index) in events" :key="index" class="space-y-0.5">
            <p class="text-sm text-ink">
              <span class="font-medium">{{ stateLabel(event.to) }}</span>
              <span v-if="event.from" class="text-ink-muted"> (from {{ stateLabel(event.from) }})</span>
            </p>
            <p v-if="event.detail" class="break-words text-sm text-ink-muted">{{ event.detail }}</p>
            <p class="text-xs text-ink-muted"><RelativeTime :value="event.at" :now="now" /></p>
          </li>
        </ol>
      </details>
    </section>

    <!-- The stop. -->
    <section v-if="server.live && server.state !== 'stopping'" class="space-y-3 rounded-panel border border-danger/40 p-4" :aria-labelledby="ids.stop">
      <h2 :id="ids.stop" class="text-lg font-semibold text-danger">Stop the server</h2>
      <p class="text-sm text-ink-muted">
        Ends the match for everyone on it.
        <template v-if="scope === 'owner'">Its settings stay here, so you can start it again. {{ STOPPING_STILL_COUNTS }}</template>
      </p>
      <button
        v-if="!confirmingStop"
        ref="stopButton"
        type="button"
        :disabled="busy"
        class="rounded-control border border-danger px-4 py-2 text-sm font-medium text-danger transition hover:bg-danger/10 disabled:opacity-40"
        @click="beginStop"
      >
        Stop server…
      </button>
      <form v-else class="space-y-3" @submit.prevent="confirmStop">
        <p ref="stopWarning" tabindex="-1" class="text-sm font-semibold text-danger focus:outline-none">
          Stop {{ server.settings.game_name || 'this server' }}? {{ stopConsequence }}
        </p>
        <div class="flex flex-wrap gap-2">
          <button
            type="submit"
            :disabled="busy"
            class="rounded-control bg-danger px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-danger/90 disabled:opacity-40"
          >
            {{ busy ? 'Stopping…' : 'Stop server now' }}
          </button>
          <button
            type="button"
            :disabled="busy"
            class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:opacity-40"
            @click="cancelStop"
          >
            Cancel
          </button>
        </div>
      </form>
    </section>
  </div>
</template>

<script setup lang="ts">
// A hosted server page's Overview tab: where the server is in its life
// (requested -> starting -> running, then stopping -> stopped, or failed),
// the game data it runs on with the owner's Update when its data set has
// newer (POST /api/me/servers/<id>/update, which restarts it), its listing
// (the owner's with the pushes its console lacks, MyServerCard embedded,
// from GET /api/me/hosts), its timeline and, while it is live, its stop (the
// owner's POST /api/me/servers/<id>/stop, the admin's POST
// /api/admin/hosted/<id>/stop).
import { computed, nextTick, onUnmounted, ref, watch } from 'vue';
import MyServerCard from '../../components/MyServerCard.vue';
import RelativeTime from '../../components/RelativeTime.vue';
import { stopFleetServer } from '../../api/admin';
import { fetchDataSets } from '../../api/dataSets';
import { fetchMyServers, STOPPING_STILL_COUNTS, stopHostedServer, updateHostedServer } from '../../api/hostedServer';
import { listMyServers } from '../../api/myServers';
import { usePolling } from '../../composables/usePolling';
import type { HostedEvent, HostedServer, PinnedTree } from '../../types/hostedServer';
import type { MyServer } from '../../types/myServers';
import {
  OWNER_START_STEPS,
  ownerStateLabel,
  ownerStepIndex,
  stateDescription,
  stateLabel,
} from '../../utils/hostedState';

const props = defineProps<{
  server: HostedServer;
  scope: 'owner' | 'admin';
  // The page's clock (ms).
  now: number;
}>();

const emit = defineEmits<{
  // The stop went in: the server as the service answered it.
  (e: 'changed', server: HostedServer | null): void;
  // A refusal said the server is gone or ended: read it again.
  (e: 'gone'): void;
  // A request was refused the session.
  (e: 'session-ended'): void;
}>();

// The owner's listings refresh on this period while the server runs.
const LISTING_POLL_MS = 10_000;

const ids = computed(() => {
  const base = `hosted-server-${props.server.id}`;
  return {
    progress: `${base}-progress`,
    data: `${base}-data`,
    listing: `${base}-listing`,
    timeline: `${base}-timeline`,
    stop: `${base}-stop`,
  };
});

const stopButton = ref<HTMLButtonElement | null>(null);
const stopWarning = ref<HTMLElement | null>(null);
const updateButton = ref<HTMLButtonElement | null>(null);
const updateWarning = ref<HTMLElement | null>(null);
const busy = ref(false);
const status = ref('');
const alertText = ref('');
const confirmingStop = ref(false);
const confirmingUpdate = ref(false);

const events = computed<HostedEvent[]>(() => props.server.events ?? []);

// The data the server runs on, and what its data set holds now.
const gameName = computed(() => props.server.data_set?.display_name || props.server.game_display_name || 'Game');
const expansionName = computed(() => props.server.expansion?.display_name || props.server.expansion_display_name || 'Expansion');

function dataVersionText(tree: PinnedTree | null): string {
  if (!tree) return 'Not placed on data yet';
  return tree.version ? `version ${tree.version}` : `tree ${tree.tree_id}`;
}

// The data set's (and expansion's) current versions, read when an update
// waits so the callout can name what is new: the owner's from GET
// /api/me/servers, the admin's from GET /api/admin/data-sets.
const current = ref<{ base: string; expansion: string } | null>(null);
async function readCurrentVersions() {
  current.value = null;
  if (!props.server.update_available) return;
  const dataSetId = props.server.data_set?.id;
  const expansionId = props.server.expansion?.id;
  if (props.scope === 'owner') {
    const result = await fetchMyServers();
    if (!result.ok) return;
    const dataSet = result.value.data_sets.find((d) => d.id === dataSetId);
    const expansion = dataSet?.expansions.find((e) => e.id === expansionId);
    current.value = { base: dataSet?.version ?? '', expansion: expansion?.version ?? '' };
    return;
  }
  const result = await fetchDataSets();
  if (!result.ok) return;
  const versionOf = (id: number | undefined) => result.value.data_sets.find((d) => d.id === id)?.current_tree?.version ?? '';
  current.value = { base: versionOf(dataSetId), expansion: versionOf(expansionId) };
}
watch(() => props.server.update_available, readCurrentVersions, { immediate: true });

// "Joint Operations 1.7.5.9 is available." (and the expansion's, when it is
// the newer one); without the versions, that newer data is.
const updateText = computed(() => {
  const now = current.value;
  const newer: string[] = [];
  if (now) {
    if (now.base && now.base !== (props.server.data.base?.version ?? '')) newer.push(`${gameName.value} ${now.base}`);
    if (now.expansion && now.expansion !== (props.server.data.expansion?.version ?? '')) {
      newer.push(`${expansionName.value} ${now.expansion}`);
    }
  }
  if (newer.length === 0) return `Newer data for ${gameName.value} is available.`;
  return `${newer.join(' and ')} ${newer.length === 1 ? 'is' : 'are'} available.`;
});

// The server's own row among the owner's listed servers.
const listing = ref<MyServer | null>(null);
const listingPoll = usePolling({
  immediate: props.scope === 'owner',
  async read(signal) {
    if (props.scope !== 'owner' || !props.server.live) return true;
    const result = await listMyServers(signal);
    if (signal.aborted) return false;
    if (!result.ok) {
      if (result.error.code === 'unauthorized') emit('session-ended');
      return false;
    }
    const rid = props.server.listing?.rid;
    listing.value =
      (rid !== undefined ? result.value.find((h) => h.rid === rid) : undefined) ??
      result.value.find((h) => h.hosted_server_id === props.server.id) ??
      null;
    return true;
  },
  next: () => (props.scope === 'owner' && props.server.live ? LISTING_POLL_MS : null),
});

// A push to the listing: its name and the like come back with the server's
// next update, so look again shortly.
let listingTimer: ReturnType<typeof setTimeout> | undefined;
function listingSoon() {
  if (listingTimer) clearTimeout(listingTimer);
  listingTimer = setTimeout(() => void listingPoll.poke(), 1500);
}
onUnmounted(() => {
  if (listingTimer) clearTimeout(listingTimer);
});

// The listing as shown: the account's listed row when the page has it (read
// every few seconds, so a rename shows soon), else the server's own.
const shownListing = computed(() => {
  const row = listing.value;
  const own = props.server.listing;
  if (row) {
    return {
      name: row.server_name || own?.server_name || props.server.settings.game_name,
      message: row.msg,
      mission: row.mission_name || own?.mission_name || '',
      players: row.players,
      maxPlayers: row.max_players,
      address: `${row.host_ip}:${row.host_port}`,
    };
  }
  if (!own) return null;
  return {
    name: own.server_name || props.server.settings.game_name,
    message: '',
    mission: own.mission_name,
    players: own.players,
    maxPlayers: own.max_players,
    address: `${own.host_ip}:${own.host_port}`,
  };
});

// The stop's line lasts until the server has ended (its state then says so).
watch(
  () => props.server.live,
  (live) => {
    if (!live) {
      status.value = '';
      listing.value = null;
    }
  },
);

const stepIndex = computed(() => ownerStepIndex(props.server.state));

function stepBarClass(index: number): string {
  if (index < stepIndex.value) return 'bg-online';
  if (index > stepIndex.value) return 'bg-hover';
  return props.server.state === 'running' ? 'bg-online' : 'bg-warn animate-pulse';
}

function stepSuffix(index: number): string {
  if (index < stepIndex.value) return ', done';
  if (index > stepIndex.value) return ', to come';
  return ', current';
}

const stateNoteClass = computed(() => {
  switch (props.server.state) {
    case 'running':
      return 'border-online/40 bg-online/10 text-online';
    case 'failed':
      return 'border-danger/40 bg-danger/10 text-danger';
    case 'stopped':
      return 'border-border bg-surface text-ink-muted';
    default:
      return 'border-warn/40 bg-warn/10 text-warn';
  }
});

const stopConsequence = computed(() => {
  const players = shownListing.value?.players;
  if (players === undefined) return 'It is not listed yet, so no one is playing on it.';
  return players === 0
    ? 'No one is playing on it right now.'
    : `The match ends at once for the ${players} player${players === 1 ? '' : 's'} on it.`;
});

async function beginUpdate() {
  confirmingUpdate.value = true;
  status.value = '';
  alertText.value = '';
  await nextTick();
  updateWarning.value?.focus();
}

async function cancelUpdate() {
  confirmingUpdate.value = false;
  await nextTick();
  updateButton.value?.focus();
}

// The owner's update: the service restarts the server on its data set's
// current data (its players drop); the reply shows at once and the page
// follows it from there.
async function confirmUpdate() {
  if (busy.value) return;
  busy.value = true;
  status.value = '';
  alertText.value = '';
  const result = await updateHostedServer(props.server.id);
  busy.value = false;
  confirmingUpdate.value = false;
  if (!result.ok) {
    alertText.value = result.error.message;
    if (result.error.code === 'unauthorized') emit('session-ended');
    else if (result.error.code === 'not_found' || result.error.code === 'not_live' || result.error.code === 'up_to_date') emit('gone');
    await nextTick();
    updateButton.value?.focus();
    return;
  }
  status.value = `Update requested: ${props.server.settings.game_name || 'the server'} restarts on the current data shortly.`;
  emit('changed', result.value);
  await nextTick();
  document.getElementById(ids.value.data)?.focus();
}

async function beginStop() {
  confirmingStop.value = true;
  status.value = '';
  alertText.value = '';
  await nextTick();
  stopWarning.value?.focus();
}

async function cancelStop() {
  confirmingStop.value = false;
  await nextTick();
  stopButton.value?.focus();
}

async function confirmStop() {
  if (busy.value) return;
  busy.value = true;
  status.value = '';
  alertText.value = '';
  const result = props.scope === 'admin' ? await stopFleetServer(props.server.id) : await stopHostedServer(props.server.id);
  busy.value = false;
  confirmingStop.value = false;
  if (!result.ok) {
    alertText.value = result.error.message;
    if (result.error.code === 'unauthorized') emit('session-ended');
    else if (result.error.code === 'not_found' || result.error.code === 'not_live') emit('gone');
    await nextTick();
    stopButton.value?.focus();
    return;
  }
  const stopped = result.value?.state === 'stopped';
  const name = props.server.settings.game_name || 'The server';
  status.value = stopped ? `${name} has stopped.` : `${name} is stopping.`;
  emit('changed', result.value);
  // The stop section goes with the stopping state: the keyboard goes to the
  // state section's heading.
  await nextTick();
  document.getElementById(ids.value.progress)?.focus();
}
</script>
