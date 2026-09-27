<template>
  <main class="bg-surface">
    <section class="mx-auto max-w-3xl px-6 pt-24 pb-28">
      <p class="text-xs uppercase tracking-[0.3em] text-accent">NovaWorld</p>
      <h1 class="mt-4 text-4xl font-bold text-ink sm:text-5xl">
        Joint Operations and Delta Force Xtreme 2, back online.
      </h1>
      <p class="mt-5 max-w-2xl text-base text-ink-muted">
        OpenNova runs the NovaWorld servers these games connect to, and is rebuilding the engine
        they run on as open source.
      </p>

      <div class="mt-8 flex flex-wrap items-center gap-3">
        <a
          :href="releasesUrl"
          target="_blank"
          rel="noopener"
          class="rounded-control bg-accent px-5 py-2.5 text-sm font-semibold text-on-accent transition hover:bg-accent/90"
        >
          Download OpenNova
        </a>
        <RouterLink
          to="/lobby"
          class="rounded-control border border-border-strong px-5 py-2.5 text-sm font-semibold text-ink transition hover:border-accent"
        >
          View lobbies
        </RouterLink>
      </div>

      <p class="mt-4 text-xs text-ink-muted">
        Windows. Pre-1.0 and experimental. Playing retail Joint Operations needs an existing
        purchased copy.
      </p>

      <p class="mt-12 flex items-center gap-2 text-sm text-ink-muted">
        <template v-if="statsLoading">Checking the network…</template>
        <template v-else-if="statsError">Network status unavailable.</template>
        <template v-else-if="stats.players || stats.lobbies">
          <span class="inline-block h-2 w-2 rounded-full bg-online"></span>
          {{ statsDisplay.players }} {{ stats.players === 1 ? 'player' : 'players' }} across
          {{ statsDisplay.lobbies }} {{ stats.lobbies === 1 ? 'lobby' : 'lobbies' }} right now.
        </template>
        <template v-else>No one is hosting right now.</template>
      </p>
    </section>
  </main>
</template>

<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue';
import { RouterLink } from 'vue-router';
import type { AxiosError } from 'axios';
import { fetchStats } from '../api/stats';

const releasesUrl = 'https://github.com/opennova-net/opennova/releases';

const statsLoading = ref(true);
const statsError = ref('');
const stats = reactive({ lobbies: 0, players: 0 });

const statsDisplay = computed(() => ({
  lobbies: stats.lobbies.toLocaleString(),
  players: stats.players.toLocaleString(),
}));

onMounted(async () => {
  const statsController = new AbortController();
  try {
    const fetched = await fetchStats(statsController.signal);
    stats.lobbies = fetched.lobbies;
    stats.players = fetched.players;
  } catch (error) {
    console.error('Failed to fetch stats', error);
    statsError.value = parseErrorMessage(error, 'Unable to load network status right now.');
  } finally {
    statsLoading.value = false;
  }
});

function parseErrorMessage(error: unknown, fallback: string): string {
  if (!error) {
    return fallback;
  }
  const axiosError = error as AxiosError<{ error?: string; detail?: string }>;
  const detail = axiosError.response?.data?.detail || axiosError.response?.data?.error;
  if (detail) {
    return `${fallback} (${detail})`;
  }
  if (axiosError.message) {
    return `${fallback} (${axiosError.message})`;
  }
  return fallback;
}
</script>
