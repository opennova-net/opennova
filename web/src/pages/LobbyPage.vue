<template>
  <section class="page space-y-6 py-10 sm:py-14">
    <PageHeader title="Servers" refreshable :loaded-at="oursAt" :now="now" :loading="loading" @refresh="refresh">
      <template #meta>
        <span>{{ rows.length }} {{ rows.length === 1 ? 'server' : 'servers' }}</span>
        <span v-if="novalogic?.enabled && novalogicAt !== null" class="novalogic-age">NovaLogic's list from <RelativeTime :value="novalogicAt" :now="now" /></span>
      </template>
      <template #actions>
        <RouterLink to="/play#online" class="btn-secondary">How to join</RouterLink>
      </template>
    </PageHeader>

    <Notice v-if="trouble" tone="warn" role="status" class="lobby-trouble">{{ trouble }}</Notice>

    <ServerTable :rows="rows" directory :empty="emptyText" caption="Servers on OpenNova and NovaLogic" />
  </section>
</template>

<script setup lang="ts">
// /lobby: every server on our NovaWorld and on NovaLogic's (the service's copy
// of their list) in one table. Each source is read every 5 s on its own and
// keeps its last list when a read fails; one notice says which is in trouble.
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from '../components/RelativeTime.vue';
import ServerTable from '../components/ServerTable.vue';
import Notice from '../components/ui/Notice.vue';
import PageHeader from '../components/ui/PageHeader.vue';
import { fetchLobbies, fetchNovaLogicLobbies } from '../api/lobbies';
import { usePolling } from '../composables/usePolling';
import type { LobbyGroup, NovaLogicLobbies, ServerRow } from '../types/lobby';
import { titleName } from '../utils/games';

const POLL_MS = 5000;

const games = ref<LobbyGroup[]>([]);
const novalogic = ref<NovaLogicLobbies | null>(null);
const oursAt = ref<number | null>(null);
const novalogicAt = ref<number | null>(null);
const oursError = ref(false);
const novalogicError = ref(false);
const now = ref(Date.now());

const rows = computed<ServerRow[]>(() => [
  ...games.value.flatMap((game) => game.hosts.map((host): ServerRow => ({
    key: `opennova-${host.id}`, to: `/lobby/${host.id}`, source: 'opennova', game: game.slug, gameName: game.displayName,
    serverName: host.serverName, mission: host.mission || '', gameType: host.gameType || '',
    players: host.players, maxPlayers: host.maxPlayers || 0, expansion: host.expansion || '', password: Boolean(host.locked),
  }))),
  ...(novalogic.value?.enabled && novalogicAt.value !== null ? novalogic.value.servers.map((server, index): ServerRow => ({
    key: `novalogic-${server.id}-${index}`, to: `/lobby/novalogic/${server.id}`, source: 'novalogic',
    game: 'jop_2_consumer', gameName: titleName('jo'), serverName: server.serverName, mission: server.mission,
    gameType: server.gameType, players: server.players, maxPlayers: server.maxPlayers, country: server.country,
    expansion: server.expansion, password: server.password, locked: server.locked, punkBuster: server.punkBuster,
  })) : []),
]);

// Which source is in trouble, in one line; '' while both answer.
const trouble = computed(() => {
  const lines: string[] = [];
  if (oursError.value) lines.push(oursAt.value === null ? 'Our server list could not be read.' : 'Our server list could not be refreshed.');
  if (novalogic.value?.enabled !== false) {
    if (novalogicError.value) lines.push(novalogicAt.value === null ? "NovaLogic's list could not be read." : "NovaLogic's list could not be refreshed.");
    else if (novalogic.value?.failing) lines.push("NovaLogic's NovaWorld can't be reached right now.");
  }
  return lines.join(' ');
});

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
      if (!signal.aborted) {
        oursError.value = true;
        now.value = Date.now();
      }
      return false;
    }
  },
  next: () => POLL_MS,
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
      if (!signal.aborted) {
        novalogicError.value = true;
        now.value = Date.now();
      }
      return false;
    }
  },
  next: () => POLL_MS,
});
const loading = computed(() => oursPoll.loading.value || novalogicPoll.loading.value);
const emptyText = computed(() => {
  if (loading.value) return 'Checking for servers…';
  if (novalogic.value?.enabled && novalogicAt.value === null && !trouble.value) return "No servers yet. Waiting for NovaLogic's list.";
  return 'No servers right now.';
});
function refresh() {
  void oursPoll.refresh();
  void novalogicPoll.refresh();
}
</script>
