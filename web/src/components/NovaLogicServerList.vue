<template>
  <section class="space-y-4" aria-labelledby="novalogic-servers">
    <div class="flex items-center justify-between gap-4">
      <h2 id="novalogic-servers" class="text-2xl font-semibold text-ink">NovaLogic's NovaWorld</h2>
      <span v-if="updatedAt !== null" class="shrink-0 text-sm text-ink-muted">
        {{ list.servers.length }} {{ list.servers.length === 1 ? 'server' : 'servers' }}
      </span>
    </div>
    <p class="text-sm text-ink-muted">
      Joint Operations servers listed on NovaLogic's own NovaWorld. To join one, connect your game to NovaLogic's
      NovaWorld instead of ours.
    </p>

    <template v-if="updatedAt !== null">
      <ServerTable :rows="rows" empty="No servers are listed there right now." caption="The servers on NovaLogic's NovaWorld" />
      <p class="text-xs text-ink-muted">
        Updated <RelativeTime :value="updatedAt" :now="now" /><template v-if="list.failing">. NovaLogic's NovaWorld can't be reached right now, so this is the last list we got.</template>
      </p>
    </template>
    <p v-else-if="list.failing" class="text-sm text-ink-muted">NovaLogic's NovaWorld can't be reached right now.</p>
    <p v-else class="text-sm text-ink-muted">Checking NovaLogic's NovaWorld...</p>
  </section>
</template>

<script setup lang="ts">
// The Lobbies page's section for the servers on NovaLogic's NovaWorld (GET
// /api/lobbies/novalogic): the service's copy of NovaLogic's list, with how long ago
// it was fetched. Each row opens the server's page (NovaLogicServerPage.vue).
import { computed } from 'vue';
import RelativeTime from './RelativeTime.vue';
import ServerTable from './ServerTable.vue';
import type { NovaLogicLobbies, ServerRow } from '../types/lobby';

const props = defineProps<{
  list: NovaLogicLobbies;
  // When the service fetched the list (epoch ms, by the page's clock); null before the first.
  updatedAt: number | null;
  // The page's clock (ms).
  now: number;
}>();

const rows = computed<ServerRow[]>(() =>
  props.list.servers.map((server, i) => ({
    key: `${server.id}-${i}`,
    to: `/lobby/novalogic/${server.id}`,
    serverName: server.serverName,
    mission: server.mission,
    gameType: server.gameType,
    players: server.players,
    maxPlayers: server.maxPlayers,
    country: server.country,
    expansion: server.expansion,
    password: server.password,
    locked: server.locked,
    punkBuster: server.punkBuster,
  })),
);
</script>
