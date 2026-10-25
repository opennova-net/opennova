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
      <div v-if="list.servers.length > 0" class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border text-sm text-ink">
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-2">Server</th>
              <th scope="col" class="hidden px-4 py-2 sm:table-cell">Mission</th>
              <th scope="col" class="px-4 py-2 text-right">Players</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border">
            <tr v-for="(server, i) in list.servers" :key="`${server.id}-${i}`">
              <th scope="row" class="px-4 py-2 text-left font-medium">
                <span class="break-all">{{ server.serverName || 'Unnamed server' }}</span>
                <span v-if="server.locked" class="ml-2 rounded-control bg-hover px-2 py-0.5 text-xs font-normal text-ink-muted">Password</span>
                <span v-if="missionText(server)" class="mt-1 block text-xs font-normal text-ink-muted sm:hidden">{{ missionText(server) }}</span>
              </th>
              <td class="hidden px-4 py-2 sm:table-cell">{{ missionText(server) }}</td>
              <td class="whitespace-nowrap px-4 py-2 text-right font-mono text-xs">{{ server.players }} / {{ server.maxPlayers }}</td>
            </tr>
          </tbody>
        </table>
      </div>
      <p v-else class="text-sm text-ink-muted">No servers are listed there right now.</p>
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
// it was fetched. Rows link nowhere: the site has no page for another service's server.
import RelativeTime from './RelativeTime.vue';
import type { NovaLogicLobbies, NovaLogicServer } from '../types/lobby';

defineProps<{
  list: NovaLogicLobbies;
  // When the service fetched the list (epoch ms, by the page's clock); null before the first.
  updatedAt: number | null;
  // The page's clock (ms).
  now: number;
}>();

// "AS - Kombang Valley (AAS)", or the mission or the game type alone.
function missionText(server: NovaLogicServer): string {
  if (server.mission && server.gameType) return `${server.mission} (${server.gameType})`;
  return server.mission || server.gameType;
}
</script>
