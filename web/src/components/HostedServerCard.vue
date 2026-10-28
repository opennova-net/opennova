<template>
  <article :id="base" class="card flex flex-col gap-3 p-4 sm:flex-row sm:items-center sm:justify-between" :aria-labelledby="`${base}-heading`">
    <div class="min-w-0 space-y-1">
      <h3 :id="`${base}-heading`" class="text-base">
        <ServerLink :name="server.settings.game_name" :id="server.id" scope="owner" />
      </h3>
      <p class="flex flex-wrap items-center gap-x-3 gap-y-1 text-sm text-ink-muted">
        <Badge :tone="serverStateTone(server.state)">{{ ownerStateLabel(server.state) }}</Badge>
        <span>{{ gameName(server) }}<template v-if="expansionLabel"> · {{ expansionLabel }}</template></span>
        <span v-if="server.own_machine">Dedicated machine</span>
        <span v-if="server.live && players">{{ players.players }} of {{ players.max }} players</span>
        <span v-if="server.live && mission" class="break-words">{{ mission }}</span>
        <span v-if="!server.live && server.ended_at">Ended <RelativeTime :value="server.ended_at" :now="now" /></span>
      </p>
      <p v-if="phaseLine" class="hosted-phase text-sm text-ink">
        {{ phaseLine }}<span v-if="slow" class="font-semibold text-warn">{{ ` ${SLOW_PHASE_TEXT}` }}</span>
      </p>
      <p v-if="server.state === 'failed' && server.failure_reason" class="break-words text-sm text-danger">{{ server.failure_reason }}</p>
      <p v-else-if="!server.live && server.stopped_because" class="hosted-stopped-because text-sm text-ink-muted">
        {{ stoppedBecauseText(server.stopped_because, 'owner') }}
      </p>
    </div>
    <div class="flex shrink-0 flex-wrap gap-2">
      <RouterLink :to="detailPath" class="btn-secondary btn-sm">
        Manage<span class="sr-only">{{ ` ${server.settings.game_name}` }}</span>
      </RouterLink>
      <RouterLink v-if="server.state === 'running'" :to="`${detailPath}/console`" class="btn-secondary btn-sm">
        Console<span class="sr-only"> of {{ server.settings.game_name }}</span>
      </RouterLink>
      <RouterLink
        v-if="!server.live && canStart"
        :to="{ path: '/me/servers/new', query: { from: String(server.id) } }"
        class="btn-secondary btn-sm"
      >
        {{ server.state === 'failed' ? 'Try again' : 'Start again' }}<span class="sr-only">: {{ server.settings.game_name }}</span>
      </RouterLink>
    </div>
  </article>
</template>

<script setup lang="ts">
// One of the account's hosted servers on its Servers page: its name (to its
// page), state, game, players and map while live, where a start stands or why
// its last run ended, and Manage, Console, or a start again with its settings.
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from './RelativeTime.vue';
import ServerLink from './links/ServerLink.vue';
import Badge from './ui/Badge.vue';
import type { HostedServer } from '../types/hostedServer';
import type { MyServer } from '../types/myServers';
import { gameName } from '../utils/games';
import { SLOW_PHASE_TEXT, ownerStateLabel, phaseSlow, phaseText, stoppedBecauseText } from '../utils/hostedState';
import { serverStateTone } from '../utils/tone';

const props = defineProps<{
  server: HostedServer;
  // The page's clock (ms).
  now: number;
  // The server's own row among the account's listed servers, while it is
  // live and listed (read every few seconds, so fresher than the server's).
  listing?: MyServer | null;
  // A new server may start now (the "Start again" link).
  canStart?: boolean;
}>();

const base = computed(() => `hosted-server-${props.server.id}`);
const detailPath = computed(() => `/me/servers/${props.server.id}`);
const expansionLabel = computed(() => props.server.expansion?.display_name || props.server.expansion_display_name);

const players = computed(() => {
  const row = props.listing ?? props.server.listing;
  return row ? { players: row.players, max: row.max_players } : null;
});
const mission = computed(() => props.listing?.mission_name || props.server.listing?.mission_name || '');

const phaseLine = computed(() => (props.server.live && props.server.phase ? phaseText(props.server.phase) : ''));
const slow = computed(() => phaseSlow(props.server, props.now));
</script>
