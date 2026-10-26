<template>
  <section class="page py-10 sm:py-14">
    <header class="flex flex-wrap items-end justify-between gap-5">
      <div>
        <p class="eyebrow">NovaWorld</p>
        <h1 class="mt-3 text-4xl font-semibold tracking-tight text-ink-bright">Servers</h1>
        <p class="mt-4 max-w-xl text-sm leading-relaxed text-ink-muted">Servers on OpenNova's NovaWorld and NovaLogic's. Open one to see its players and settings, then join from your game's server browser.</p>
      </div>
      <div class="flex flex-wrap items-center gap-3">
        <RouterLink to="/play#online" class="btn-secondary">How to join</RouterLink>
        <button type="button" class="btn-secondary" :disabled="loading" @click="refresh">{{ loading ? 'Refreshing…' : 'Refresh' }}</button>
      </div>
    </header>

    <div class="my-8 grid gap-3 sm:grid-cols-2" aria-label="Listing sources">
      <section class="card p-4" aria-labelledby="opennova-source">
        <h2 id="opennova-source" class="text-sm font-semibold text-ink">OpenNova's NovaWorld</h2>
        <p v-if="oursAt !== null" class="mt-2 text-xs text-ink-muted">{{ ownCount }} {{ ownCount === 1 ? 'server' : 'servers' }} · Updated <RelativeTime :value="oursAt" :now="now" />
        </p>
        <p v-else-if="!oursError" class="mt-2 text-xs text-ink-muted">Checking our NovaWorld…</p>
        <p v-if="oursError" role="status" class="mt-2 text-xs text-warn">{{ oursAt === null ? 'Our server list could not be loaded.' : 'Our last refresh failed. Showing the last list received.' }} We keep trying.</p>
      </section>
      <section class="card p-4" aria-labelledby="novalogic-source">
        <h2 id="novalogic-source" class="text-sm font-semibold text-ink">NovaLogic's NovaWorld</h2>
        <p v-if="novalogic && !novalogic.enabled" class="mt-2 text-xs text-ink-muted">This deployment does not list NovaLogic's servers.</p>
        <template v-else>
          <p v-if="novalogicAt !== null" class="mt-2 text-xs text-ink-muted">{{ novalogic?.servers.length ?? 0 }} {{ novalogic?.servers.length === 1 ? 'server' : 'servers' }} · Updated <RelativeTime :value="novalogicAt" :now="now" />
          </p>
          <p v-if="novalogicError" role="status" class="mt-2 text-xs text-warn">{{ novalogicAt === null ? 'The NovaLogic list could not be loaded from our service.' : 'Our last refresh of the NovaLogic list failed. Showing the last list received.' }} We keep trying.</p>
          <p v-else-if="novalogic?.failing" role="status" class="mt-2 text-xs text-warn">NovaLogic's NovaWorld can't be reached right now.<template v-if="novalogicAt !== null"> This is the last list our service received.</template>
          </p>
          <p v-else-if="novalogicAt === null" class="mt-2 text-xs text-ink-muted">Checking NovaLogic's NovaWorld…</p>
        </template>
      </section>
    </div>

    <p v-if="loading && oursAt === null && novalogicAt === null" class="mb-5 text-sm text-ink-muted" role="status">Loading server listings…</p>
    <ServerTable :rows="rows" directory :empty="emptyText" caption="Servers on OpenNova and NovaLogic" />
    <div class="mt-8 flex flex-wrap items-center justify-between gap-3 border-t border-border pt-6 text-sm text-ink-muted">
      <p>Each listing names the NovaWorld service to select in-game.</p>
      <RouterLink to="/support" class="link">Host your own server</RouterLink>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from '../components/RelativeTime.vue';
import ServerTable from '../components/ServerTable.vue';
import { fetchLobbies, fetchNovaLogicLobbies } from '../api/lobbies';
import { usePolling } from '../composables/usePolling';
import type { LobbyGroup, NovaLogicLobbies, ServerRow } from '../types/lobby';

const games = ref<LobbyGroup[]>([]);
const novalogic = ref<NovaLogicLobbies | null>(null);
const oursAt = ref<number | null>(null);
const novalogicAt = ref<number | null>(null);
const oursError = ref(false);
const novalogicError = ref(false);
const now = ref(Date.now());
const ownCount = computed(() => games.value.reduce((total, game) => total + game.hosts.length, 0));
const rows = computed<ServerRow[]>(() => [
  ...games.value.flatMap((game) => game.hosts.map((host): ServerRow => ({
    key: `opennova-${host.id}`, to: `/lobby/${host.id}`, source: 'opennova', game: game.slug, gameName: game.displayName,
    serverName: host.serverName, mission: host.mission || '', gameType: host.gameType || '',
    players: host.players, maxPlayers: host.maxPlayers || 0, expansion: host.expansion || '', password: Boolean(host.locked),
  }))),
  ...(novalogic.value?.enabled && novalogicAt.value !== null ? novalogic.value.servers.map((server, index): ServerRow => ({
    key: `novalogic-${server.id}-${index}`, to: `/lobby/novalogic/${server.id}`, source: 'novalogic',
    game: 'jop_2_consumer', gameName: 'Joint Operations', serverName: server.serverName, mission: server.mission,
    gameType: server.gameType, players: server.players, maxPlayers: server.maxPlayers, country: server.country,
    expansion: server.expansion, password: server.password, locked: server.locked, punkBuster: server.punkBuster,
  })) : []),
]);

// Independent, visibility-aware reads retain the last successful list. A failure
// of either source never blocks the other or silently makes old data look fresh.
const oursPoll = usePolling({
  async read(signal) {
    try {
      const result = await fetchLobbies(signal);
      if (signal.aborted) return false;
      games.value = result.games;
      oursAt.value = now.value = Date.now();
      oursError.value = false;
      return true;
    } catch {
      if (!signal.aborted) { oursError.value = true; now.value = Date.now(); }
      return false;
    }
  },
  next: () => 5000,
});
const novalogicPoll = usePolling({
  async read(signal) {
    try {
      const result = await fetchNovaLogicLobbies(signal);
      if (signal.aborted) return false;
      novalogic.value = result;
      now.value = Date.now();
      const fetched = result.ageSeconds !== null ? now.value - result.ageSeconds * 1000 : result.fetchedAt ? Date.parse(result.fetchedAt) : null;
      novalogicAt.value = fetched !== null && Number.isFinite(fetched) ? fetched : null;
      novalogicError.value = false;
      return true;
    } catch {
      if (!signal.aborted) { novalogicError.value = true; now.value = Date.now(); }
      return false;
    }
  },
  next: () => 5000,
});
const loading = computed(() => oursPoll.loading.value || novalogicPoll.loading.value);
const emptyText = computed(() => {
  if (loading.value) return 'Checking for server listings…';
  if (oursError.value || novalogicError.value || novalogic.value?.failing) return 'No server listings are available. Check the source notices above.';
  if (novalogic.value?.enabled && novalogicAt.value === null) return "No servers listed on OpenNova. Waiting for NovaLogic's first list.";
  return 'No servers are listed right now.';
});
function refresh() { void oursPoll.refresh(); void novalogicPoll.refresh(); }
</script>
