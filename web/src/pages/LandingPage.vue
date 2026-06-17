<template>
  <main class="bg-surface">
    <section class="mx-auto max-w-3xl px-6 pt-24 pb-28">
      <p class="text-xs uppercase tracking-[0.3em] text-accent">NovaWorld</p>
      <h1 class="mt-4 text-4xl font-bold text-ink sm:text-5xl">
        Joint Operations and Delta Force Xtreme 2, back online.
      </h1>
      <p class="mt-5 max-w-2xl text-base text-ink-muted">
        OpenNova runs the NovaWorld servers these games connect to. The launcher points your game
        at them and manages expansions. Your install stays stock — no patched executables.
      </p>

      <div class="mt-8 flex flex-wrap items-center gap-3">
        <a
          v-if="hasDownload"
          :href="downloadUrl"
          target="_blank"
          rel="noopener"
          class="rounded-control bg-accent px-5 py-2.5 text-sm font-semibold text-on-accent transition hover:bg-accent/90"
        >
          {{ downloadCtaText }}
        </a>
        <RouterLink
          to="/lobby"
          class="rounded-control border border-border-strong px-5 py-2.5 text-sm font-semibold text-ink transition hover:border-accent"
        >
          View lobbies
        </RouterLink>
      </div>

      <p class="mt-4 text-xs text-ink-muted">
        <span v-if="downloadMeta">{{ downloadMeta }} · </span>Windows. Requires an existing purchased copy.
      </p>
      <p v-if="launcherState.error" class="mt-3 text-xs text-danger">{{ launcherState.error }}</p>

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
import { computed, onMounted, onUnmounted, reactive, ref } from 'vue';
import { RouterLink } from 'vue-router';
import type { AxiosError } from 'axios';
import { fetchLauncherManifest } from '../api/downloads';
import { fetchStats } from '../api/stats';
import type { DownloadManifest } from '../types/downloads';

type DownloadState = {
  loading: boolean;
  error: string;
  manifest: DownloadManifest | null;
};

const launcherState = reactive<DownloadState>({ loading: true, error: '', manifest: null });

const statsLoading = ref(true);
const statsError = ref('');
const stats = reactive({ games: 0, lobbies: 0, players: 0 });

const manifest = computed(() => launcherState.manifest);
const hasDownload = computed(() => !!manifest.value?.downloadUrl);
const downloadUrl = computed(() => manifest.value?.downloadUrl || '');

const platformLabel = computed(() => {
  const m = manifest.value;
  if (!m) {
    return 'Windows';
  }
  return [m.platform || 'Windows', m.architecture].filter(Boolean).join(' ');
});

const downloadCtaText = computed(() => `Download for ${platformLabel.value}`);

const downloadMeta = computed(() => {
  const m = manifest.value;
  if (!m) {
    return '';
  }
  return [m.version ? `v${m.version}` : '', m.sizeHuman].filter(Boolean).join(' · ');
});

const statsDisplay = computed(() => ({
  lobbies: stats.lobbies.toLocaleString(),
  players: stats.players.toLocaleString(),
}));

let launcherController: AbortController | null = null;

onMounted(async () => {
  loadLauncherManifest();

  const statsController = new AbortController();
  try {
    const fetched = await fetchStats(statsController.signal);
    stats.games = fetched.games;
    stats.lobbies = fetched.lobbies;
    stats.players = fetched.players;
  } catch (error) {
    console.error('Failed to fetch stats', error);
    statsError.value = parseErrorMessage(error, 'Unable to load network status right now.');
  } finally {
    statsLoading.value = false;
  }
});

onUnmounted(() => {
  launcherController?.abort();
});

async function loadLauncherManifest() {
  launcherState.loading = true;
  launcherState.error = '';
  launcherController?.abort();
  launcherController = new AbortController();
  try {
    launcherState.manifest = await fetchLauncherManifest(launcherController.signal);
  } catch (error) {
    console.error('Failed to fetch launcher manifest', error);
    launcherState.error = parseErrorMessage(error);
  } finally {
    launcherState.loading = false;
  }
}

function parseErrorMessage(error: unknown, fallback = 'Unable to load launcher details right now.'): string {
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
