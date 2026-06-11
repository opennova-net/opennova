<template>
  <section class="px-6 pt-24 pb-16">
    <div class="mx-auto max-w-6xl">
      <header class="flex flex-wrap items-center justify-between gap-4">
        <div>
          <p class="text-sm uppercase tracking-[0.3em] text-brand-200">Game Browser</p>
          <h1 class="text-3xl font-semibold text-white">Lobbies</h1>
        </div>
        <button
          class="rounded-full border border-white/10 px-5 py-2 text-sm text-white hover:border-brand-200"
          @click="refresh"
          :disabled="state.loading"
        >
          <span v-if="state.loading">Refreshing...</span>
          <span v-else>Refresh</span>
        </button>
      </header>

      <div v-if="state.error" class="mt-6 rounded-2xl border border-red-500/30 bg-red-500/10 p-4 text-sm text-red-200">
        {{ state.error }}
      </div>

      <div v-if="state.loading && !state.error" class="mt-12 animate-pulse rounded-3xl border border-white/5 bg-white/5 p-8 text-center text-slate-300">
        Loading lobbies...
      </div>

      <div v-if="!state.loading && !state.error" class="mt-10 space-y-10">
        <div v-if="state.games.length === 0" class="rounded-3xl border border-white/5 bg-white/5 p-8 text-center text-slate-300">
          No active lobbies right now. Be the first to host!
        </div>

        <div v-for="group in state.games" :key="group.slug" class="space-y-4">
          <div class="flex items-center justify-between">
            <h2 class="text-2xl font-semibold text-white">{{ group.displayName }}</h2>
            <span class="text-sm text-slate-400">{{ group.hosts.length }} active host(s)</span>
          </div>
          <div class="grid gap-4 md:grid-cols-2">
            <LobbyCard v-for="host in group.hosts" :key="host.id" :host="host" />
          </div>
        </div>
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import { onMounted, reactive } from 'vue';
import LobbyCard from '../components/LobbyCard.vue';
import { fetchLobbies } from '../api/lobbies';
import type { LobbyGroup } from '../types/lobby';

const state = reactive({
  games: [] as LobbyGroup[],
  loading: true,
  error: ''
});

const load = async () => {
  state.loading = true;
  state.error = '';
  try {
    const { games } = await fetchLobbies();
    state.games = games;
  } catch (error) {
    state.error = 'Unable to load lobbies right now. Please try again shortly.';
  } finally {
    state.loading = false;
  }
};

const refresh = () => load();

onMounted(load);
</script>
