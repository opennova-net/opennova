<template>
  <section class="px-4 py-10 sm:px-6 sm:py-14">
    <div class="mx-auto max-w-6xl">
      <header class="flex flex-wrap items-center justify-between gap-4">
        <div>
          <p class="eyebrow">NovaWorld</p>
          <h1 class="mt-2 text-3xl font-semibold text-ink-bright">Lobbies</h1>
          <p class="mt-2 text-sm text-ink-muted">Games hosted on NovaWorld right now. Join them from the game's own server browser.</p>
        </div>
        <button
          class="btn-secondary"
          @click="refresh"
          :disabled="state.loading"
        >
          <span v-if="state.loading">Refreshing...</span>
          <span v-else>Refresh</span>
        </button>
      </header>

      <div v-if="state.error" class="mt-6 rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
        {{ state.error }}
      </div>

      <div v-if="state.loading && !state.error" class="mt-12 animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
        Loading lobbies...
      </div>

      <div v-if="!state.loading && !state.error" class="mt-10 space-y-10">
        <!-- The API returns a group per game even with no hosts, so guard on the
             total host count (not games.length) — otherwise an idle server renders
             bare game headers over empty grids. -->
        <div v-if="totalHosts === 0" class="rounded-panel border border-border bg-panel p-10 text-center">
          <p class="text-lg font-semibold text-ink">No games are being hosted right now</p>
          <p class="mt-2 text-sm text-ink-muted">
            Be the first: host one from the game, or run your own server with a
            <RouterLink to="/support" class="link">support tier</RouterLink>.
          </p>
        </div>

        <template v-else>
          <div v-for="group in state.games" :key="group.slug" class="space-y-4">
            <div class="flex items-center justify-between">
              <h2 class="text-2xl font-semibold text-ink">{{ group.displayName }}</h2>
              <span class="text-sm text-ink-muted">{{ group.hosts.length }} {{ group.hosts.length === 1 ? 'server' : 'servers' }}</span>
            </div>
            <div v-if="group.hosts.length > 0" class="grid gap-4 md:grid-cols-2">
              <LobbyCard v-for="host in group.hosts" :key="host.id" :host="host" />
            </div>
            <p v-else class="text-sm text-ink-muted">No servers right now.</p>
          </div>
        </template>
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, reactive } from 'vue';
import { RouterLink } from 'vue-router';
import LobbyCard from '../components/LobbyCard.vue';
import { fetchLobbies } from '../api/lobbies';
import type { LobbyGroup } from '../types/lobby';

const state = reactive({
  games: [] as LobbyGroup[],
  loading: true,
  error: ''
});

const totalHosts = computed(() => state.games.reduce((n, group) => n + group.hosts.length, 0));

const load = async (background = false) => {
  // Background polls update the list in place: don't toggle the loading
  // skeleton or wipe the list on a transient fetch error, or the page would
  // flash "Loading lobbies..." every poll interval.
  if (!background) {
    state.loading = true;
    state.error = '';
  }
  try {
    const { games } = await fetchLobbies();
    state.games = games;
    if (background) state.error = '';
  } catch (error) {
    if (!background) {
      state.error = 'Unable to load lobbies right now. Please try again shortly.';
    }
  } finally {
    if (!background) state.loading = false;
  }
};

const refresh = () => load();

// Poll so a game hosted after the page is open appears without a manual
// refresh. The in-game flow registers the host seconds after login, long
// after the initial onMounted fetch would have run.
const POLL_INTERVAL_MS = 5000;
let pollTimer: ReturnType<typeof setInterval> | undefined;

onMounted(() => {
  load();
  pollTimer = setInterval(() => load(true), POLL_INTERVAL_MS);
});

onUnmounted(() => {
  if (pollTimer) clearInterval(pollTimer);
});
</script>
