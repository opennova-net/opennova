<template>
  <article class="space-y-8 rounded-panel border border-border bg-panel p-4 sm:p-6" :aria-labelledby="ids.heading">
    <header class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
      <div class="min-w-0">
        <p class="text-xs uppercase tracking-[0.3em] text-ink-muted">
          {{ server.live ? 'Your hosted server' : 'Your last hosted server' }}
        </p>
        <h2
          :id="ids.heading"
          ref="heading"
          tabindex="-1"
          class="mt-1 break-words text-2xl font-semibold text-ink focus:outline-none"
        >
          {{ server.settings.game_name || 'Unnamed server' }}
        </h2>
        <p class="mt-2 flex flex-wrap items-center gap-2 text-sm text-ink-muted">
          <span :class="stateBadgeClass(server.state)">{{ stateLabel(server.state) }}</span>
          <span>{{ expansionLabel }}</span>
        </p>
      </div>
      <dl class="grid shrink-0 grid-cols-[auto_auto] gap-x-3 gap-y-1 text-xs text-ink-muted sm:text-right">
        <dt>Requested</dt>
        <dd><time :datetime="isoOf(server.created_at)" :title="localOf(server.created_at)">{{ ago(server.created_at, now) }}</time></dd>
        <template v-if="server.running_at">
          <dt>First running</dt>
          <dd><time :datetime="isoOf(server.running_at)" :title="localOf(server.running_at)">{{ ago(server.running_at, now) }}</time></dd>
        </template>
        <template v-if="server.ended_at">
          <dt>Ended</dt>
          <dd><time :datetime="isoOf(server.ended_at)" :title="localOf(server.ended_at)">{{ ago(server.ended_at, now) }}</time></dd>
        </template>
      </dl>
    </header>

    <!-- Live regions, always in the card so a screen reader hears what lands
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
      <h3 :id="ids.progress" class="text-lg font-semibold text-ink">State</h3>
      <ol v-if="stepIndex >= 0" class="flex flex-col gap-2 sm:flex-row sm:gap-1">
        <li
          v-for="(step, index) in START_STEPS"
          :key="step"
          class="flex items-center gap-2 sm:flex-1 sm:flex-col sm:items-stretch"
          :aria-current="index === stepIndex ? 'step' : undefined"
        >
          <span
            aria-hidden="true"
            class="h-2.5 w-2.5 shrink-0 rounded-full sm:h-1.5 sm:w-full"
            :class="stepBarClass(index)"
          ></span>
          <span class="text-xs" :class="index === stepIndex ? 'font-semibold text-ink' : 'text-ink-muted'">
            {{ stateLabel(step) }}<span class="sr-only">{{ stepSuffix(index) }}</span>
          </span>
        </li>
      </ol>
      <p
        class="rounded-panel border p-3 text-sm"
        :class="stateNoteClass"
      >
        <span class="font-semibold">{{ stateLabel(server.state) }}.</span>
        {{ stateDescription(server.state) }}
        <template v-if="server.state === 'failed' && server.failure_reason">
          The service's note: {{ server.failure_reason }}.
        </template>
        <template v-if="server.state === 'stopping'">This takes up to a minute.</template>
      </p>
      <p v-if="server.live && server.restarts > 0" class="text-sm text-warn">
        The service restarted it {{ server.restarts }} time{{ server.restarts === 1 ? '' : 's' }} after a crash.
        After 3 restarts without an hour of running in between, the next crash ends it as failed.
      </p>
    </section>

    <!-- The listing the server's NovaWorld account holds. -->
    <section v-if="server.live" class="space-y-3" :aria-labelledby="ids.listing">
      <h3 :id="ids.listing" class="text-lg font-semibold text-ink">Listing</h3>
      <div v-if="server.listing" class="space-y-3 rounded-panel border border-border p-4">
        <dl class="grid gap-x-6 gap-y-2 text-sm sm:grid-cols-[auto_1fr]">
          <dt class="text-ink-muted">Name</dt>
          <dd class="break-words text-ink">{{ server.listing.server_name || server.settings.game_name }}</dd>
          <dt class="text-ink-muted">Mission</dt>
          <dd class="break-words text-ink">{{ server.listing.mission_name || 'No mission yet' }}</dd>
          <dt class="text-ink-muted">Players</dt>
          <dd class="text-ink">{{ server.listing.players }} of {{ server.listing.max_players }}</dd>
          <dt class="text-ink-muted">Address</dt>
          <dd class="break-all font-mono text-ink">{{ server.listing.host_ip }}:{{ server.listing.host_port }}</dd>
        </dl>
        <p class="text-sm text-ink-muted">
          Players find it in the NovaWorld server browser. Chat, renaming and player controls:
          <RouterLink to="/me/servers" class="text-accent hover:text-ink-bright">My servers</RouterLink>.
        </p>
      </div>
      <p v-else-if="server.state === 'running'" class="text-sm text-warn">
        Not listed right now. If its listing stays gone for about a minute, the service ends the server as
        failed.
      </p>
      <p v-else class="text-sm text-ink-muted">
        Not listed yet: it appears in the server browser once the game server is up.
      </p>
    </section>

    <!-- The settings it was started with. -->
    <section class="space-y-3" :aria-labelledby="ids.settings">
      <h3 :id="ids.settings" class="text-lg font-semibold text-ink">Settings</h3>
      <dl class="grid gap-x-6 gap-y-2 text-sm sm:grid-cols-[auto_1fr]">
        <dt class="text-ink-muted">{{ FIELD_LABELS.max_players }}</dt>
        <dd class="text-ink">{{ server.settings.max_players }}</dd>
        <dt class="text-ink-muted">{{ FIELD_LABELS.game_password }}</dt>
        <dd class="flex flex-wrap items-center gap-2 text-ink">
          <template v-if="server.settings.game_password">
            <span class="font-mono">{{ showPassword ? server.settings.game_password : '••••••' }}</span>
            <button
              type="button"
              class="text-xs text-accent hover:text-ink-bright"
              :aria-pressed="showPassword ? 'true' : 'false'"
              @click="showPassword = !showPassword"
            >
              {{ showPassword ? 'Hide' : 'Show' }}<span class="sr-only"> the game password</span>
            </button>
          </template>
          <template v-else>None (public)</template>
        </dd>
        <dt class="text-ink-muted">{{ FIELD_LABELS.time_limit }}</dt>
        <dd class="text-ink">{{ server.settings.time_limit }} minutes</dd>
        <dt class="text-ink-muted">{{ FIELD_LABELS.kill_limit }}</dt>
        <dd class="text-ink">{{ limitText(server.settings.kill_limit) }}</dd>
        <dt class="text-ink-muted">{{ FIELD_LABELS.max_score }}</dt>
        <dd class="text-ink">{{ limitText(server.settings.max_score) }}</dd>
        <dt class="text-ink-muted">{{ FIELD_LABELS.respawn }}</dt>
        <dd class="text-ink">{{ server.settings.respawn }} seconds</dd>
        <dt class="text-ink-muted">{{ FIELD_LABELS.start_delay }}</dt>
        <dd class="text-ink">{{ server.settings.start_delay }} seconds</dd>
        <dt class="text-ink-muted">{{ FIELD_LABELS.team_ff }}</dt>
        <dd class="text-ink">{{ server.settings.team_ff ? 'On' : 'Off' }}</dd>
        <dt class="text-ink-muted">{{ FIELD_LABELS.expansion }}</dt>
        <dd class="text-ink">{{ expansionLabel }}</dd>
        <dt class="text-ink-muted">{{ FIELD_LABELS.missions }}</dt>
        <dd class="min-w-0 text-ink">
          <ol class="list-inside list-decimal space-y-0.5 font-mono text-xs">
            <li v-for="(mission, index) in server.settings.missions" :key="index" class="break-all">{{ mission }}</li>
          </ol>
        </dd>
      </dl>
    </section>

    <!-- PR K's console panes mount here (HostedServerConsole is the seam). -->
    <HostedServerConsole v-if="server.live" :server="server" />

    <!-- Every transition, oldest first. -->
    <section v-if="events.length > 0" class="space-y-3" :aria-labelledby="ids.timeline">
      <h3 :id="ids.timeline" class="text-lg font-semibold text-ink">Timeline</h3>
      <details :open="server.live">
        <summary class="cursor-pointer text-sm text-accent hover:text-ink-bright">
          {{ events.length }} event{{ events.length === 1 ? '' : 's' }}, oldest first
        </summary>
        <ol
          class="mt-3 max-h-96 space-y-3 overflow-y-auto border-l border-border pl-4"
          tabindex="0"
          :aria-labelledby="ids.timeline"
        >
          <li v-for="(event, index) in events" :key="index" class="space-y-0.5">
            <p class="text-sm text-ink">
              <span class="font-medium">{{ stateLabel(event.to) }}</span>
              <span v-if="event.from" class="text-ink-muted"> (from {{ stateLabel(event.from) }})</span>
            </p>
            <p v-if="event.detail" class="break-words text-sm text-ink-muted">{{ event.detail }}</p>
            <p class="text-xs text-ink-muted">
              <time :datetime="isoOf(event.at)">{{ localOf(event.at) }}</time>
            </p>
          </li>
        </ol>
      </details>
    </section>

    <!-- The owner's stop. -->
    <section
      v-if="server.live && server.state !== 'stopping'"
      class="space-y-3 rounded-panel border border-danger/40 p-4"
      :aria-labelledby="ids.stop"
    >
      <h3 :id="ids.stop" class="text-lg font-semibold text-danger">Stop the server</h3>
      <p class="text-sm text-ink-muted">
        Ends the match for everyone on it and shuts its machine down. Its settings stay on this page, so you
        can start it again.
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
          Stop {{ server.settings.game_name || 'your server' }}? {{ stopConsequence }}
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
  </article>
</template>

<script setup lang="ts">
// One hosted server as its owner sees it: where it is in its life
// (requested -> provisioning -> booting -> listing -> running, then stopping
// -> stopped, or failed), its listing, its settings, the console seam, its
// timeline and, while it is live, the owner's stop (POST /api/me/server/stop).
// The page (HostedServerPage) polls and hands the server in.
import { computed, nextTick, ref, watch } from 'vue';
import { RouterLink } from 'vue-router';
import HostedServerConsole from './HostedServerConsole.vue';
import { stopHostedServer, type HostedError } from '../api/hostedServer';
import type { HostedEvent, HostedServer } from '../types/hostedServer';
import { FIELD_LABELS } from '../utils/hostedSettings';
import {
  START_STEPS,
  stateBadgeClass,
  stateDescription,
  stateLabel,
} from '../utils/hostedState';
import { ago, isoOf, localOf } from '../utils/utcTime';

const props = defineProps<{
  server: HostedServer;
  // The page's clock (ms), for the "ago" times.
  now: number;
}>();

const emit = defineEmits<{
  // The stop went in (202): the server as the service answered it.
  (e: 'stopped', server: HostedServer | null): void;
  // A refusal the page acts on besides showing it here.
  (e: 'refused', error: HostedError): void;
}>();

const ids = computed(() => {
  const base = `hosted-server-${props.server.id}`;
  return {
    heading: `${base}-heading`,
    progress: `${base}-progress`,
    listing: `${base}-listing`,
    settings: `${base}-settings`,
    timeline: `${base}-timeline`,
    stop: `${base}-stop`,
  };
});

const heading = ref<HTMLElement | null>(null);
const stopButton = ref<HTMLButtonElement | null>(null);
const stopWarning = ref<HTMLElement | null>(null);
const busy = ref(false);
const status = ref('');
const alertText = ref('');
const confirmingStop = ref(false);
const showPassword = ref(false);

const events = computed<HostedEvent[]>(() => props.server.events ?? []);

// The stop's "stopping" line lasts until the server has ended (its state then
// says so itself).
watch(
  () => props.server.live,
  (live) => {
    if (!live) status.value = '';
  },
);

const expansionLabel = computed(() =>
  props.server.settings.expansion === 'jox01' ? 'Escalation (jox01)' : 'Joint Operations (base game)',
);

// The start step the server is on, or -1 once it is stopping or has ended.
const stepIndex = computed(() => START_STEPS.indexOf(props.server.state));

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

// KillLimit and MaxScore at 500 are the host screen's "no limit".
function limitText(value: number): string {
  return value >= 500 ? 'No limit' : String(value);
}

const stopConsequence = computed(() => {
  const listing = props.server.listing;
  if (!listing) return 'It is not listed yet, so no one is playing on it.';
  const players = listing.players;
  return players === 0
    ? 'No one is playing on it right now.'
    : `The match ends at once for the ${players} player${players === 1 ? '' : 's'} on it.`;
});

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
  const result = await stopHostedServer();
  busy.value = false;
  confirmingStop.value = false;
  if (!result.ok) {
    alertText.value = result.error.message;
    emit('refused', result.error);
    await nextTick();
    stopButton.value?.focus();
    return;
  }
  status.value =
    result.value?.state === 'stopped'
      ? 'Your server has stopped.'
      : 'Stopping your server: the match ends and its machine shuts down. This page follows it.';
  emit('stopped', result.value);
  // The stop section goes with the stopping state: the keyboard goes to the
  // card's heading.
  await nextTick();
  heading.value?.focus();
}

function focusHeading() {
  heading.value?.focus();
}

defineExpose({ focusHeading });
</script>
