<template>
  <article
    :id="`${base}`"
    class="flex flex-col gap-4 rounded-panel border border-border bg-panel p-4 sm:flex-row sm:items-start sm:justify-between sm:p-5"
    :aria-labelledby="`${base}-heading`"
  >
    <div class="min-w-0 space-y-2">
      <h3 :id="`${base}-heading`" class="break-words text-lg font-semibold text-ink">
        <RouterLink :to="detailPath" class="hover:text-ink-bright">
          {{ server.settings.game_name || 'Unnamed server' }}
        </RouterLink>
      </h3>
      <p class="flex flex-wrap items-center gap-2 text-sm text-ink-muted">
        <span :class="stateBadgeClass(server.state)">{{ ownerStateLabel(server.state) }}</span>
        <span>{{ gameLabel }}</span>
        <span v-if="expansionLabel">{{ expansionLabel }}</span>
        <span v-if="server.own_machine" class="rounded-control bg-hover px-2 py-0.5 text-xs text-ink">Own machine</span>
      </p>
      <dl class="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 text-sm">
        <template v-if="server.live && players">
          <dt class="text-ink-muted">Players</dt>
          <dd class="text-ink">{{ players.players }} of {{ players.max }}</dd>
        </template>
        <template v-if="server.live && mission">
          <dt class="text-ink-muted">Mission</dt>
          <dd class="break-words text-ink">{{ mission }}</dd>
        </template>
        <template v-if="server.live && address">
          <dt class="text-ink-muted">Address</dt>
          <dd class="break-all font-mono text-xs text-ink">{{ address }}</dd>
        </template>
        <template v-if="server.state === 'running' && server.running_at">
          <dt class="text-ink-muted">Running since</dt>
          <dd class="text-ink"><RelativeTime :value="server.running_at" :now="now" /></dd>
        </template>
        <template v-else-if="server.live">
          <dt class="text-ink-muted">Requested</dt>
          <dd class="text-ink"><RelativeTime :value="server.created_at" :now="now" /></dd>
        </template>
        <template v-if="!server.live && server.ended_at">
          <dt class="text-ink-muted">Ended</dt>
          <dd class="text-ink"><RelativeTime :value="server.ended_at" :now="now" /></dd>
        </template>
      </dl>
      <p v-if="server.state === 'failed' && server.failure_reason" class="break-words text-sm text-danger">
        {{ server.failure_reason }}
      </p>
      <p v-if="server.state === 'stopping'" class="text-xs text-ink-muted">{{ STOPPING_STILL_COUNTS }}</p>
    </div>
    <div class="flex shrink-0 flex-wrap gap-2 sm:flex-col sm:items-end">
      <RouterLink :to="detailPath" :class="secondaryButton">
        Manage<span class="sr-only"> {{ server.settings.game_name }}</span>
      </RouterLink>
      <RouterLink v-if="server.state === 'running'" :to="`${detailPath}/console`" :class="secondaryButton">
        Console<span class="sr-only"> of {{ server.settings.game_name }}</span>
      </RouterLink>
      <RouterLink
        v-if="!server.live && canStart"
        :to="{ path: '/me/servers/new', query: { from: String(server.id) } }"
        :class="secondaryButton"
      >
        {{ server.state === 'failed' ? 'Try again' : 'Start again' }}<span class="sr-only">: {{ server.settings.game_name }}</span>
      </RouterLink>
    </div>
  </article>
</template>

<script setup lang="ts">
// One of the account's hosted servers on its Servers page: its state, game,
// players and address while live, and links to its own page (Manage, its
// console while it runs) or, once it has ended, to a new start with its
// settings. Its ids carry its id, so any number of cards share a page.
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from './RelativeTime.vue';
import { STOPPING_STILL_COUNTS } from '../api/hostedServer';
import type { HostedServer } from '../types/hostedServer';
import type { MyServer } from '../types/myServers';
import { ownerStateLabel, stateBadgeClass } from '../utils/hostedState';

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

const secondaryButton =
  'rounded-control border border-border bg-raised px-3 py-1.5 text-sm font-medium text-ink transition hover:bg-hover';

const base = computed(() => `hosted-server-${props.server.id}`);
const detailPath = computed(() => `/me/servers/${props.server.id}`);
const gameLabel = computed(() => props.server.data_set?.display_name || props.server.game_display_name || 'Unknown game');
const expansionLabel = computed(() => props.server.expansion?.display_name || props.server.expansion_display_name);

const players = computed(() => {
  const row = props.listing;
  if (row) return { players: row.players, max: row.max_players };
  const own = props.server.listing;
  return own ? { players: own.players, max: own.max_players } : null;
});
const mission = computed(() => props.listing?.mission_name || props.server.listing?.mission_name || '');
const address = computed(() => {
  const row = props.listing ?? props.server.listing;
  return row ? `${row.host_ip}:${row.host_port}` : '';
});
</script>
