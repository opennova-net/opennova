<template>
  <main class="space-y-24 bg-slate-950">
    <section class="px-6 pt-24">
      <div class="mx-auto grid max-w-6xl items-start gap-12 lg:grid-cols-[1.1fr_1fr]">
        <div>
          <p class="text-sm uppercase tracking-[0.3em] text-brand-200">NovaWorld revived</p>
          <h1 class="mt-4 text-4xl font-bold text-white sm:text-5xl lg:text-6xl">Play Joint Ops & Delta Force Xtreme 2 online again.</h1>
          <p class="mt-6 max-w-2xl text-lg text-slate-300">
            OpenNova restores and preserves the classic Novalogic gaming experience. Our launcher applies game-improving patches for you, and ships an expansion manager with a
            curated catalog so the games stay balanced and stable.
          </p>
          <div class="mt-8 flex flex-wrap gap-4">
            <RouterLink to="/lobby" class="rounded-full bg-brand-500 px-6 py-3 font-semibold text-white shadow-lg transition hover:bg-brand-400">
              View Lobbies
            </RouterLink>
            <RouterLink
              to="/register"
              class="rounded-full border border-white/20 px-6 py-3 font-semibold text-white transition hover:border-brand-200"
            >
              Create Account
            </RouterLink>
          </div>

        </div>

        <div class="relative mt-12 lg:mt-0">
          <div class="absolute -top-8 right-0 h-40 w-40 rounded-full bg-brand-500/10 blur-3xl lg:-right-8"></div>
          <div class="relative rounded-3xl border border-white/5 bg-gradient-to-br from-brand-500/10 via-slate-900 to-slate-950 p-6 shadow-2xl">
            <h2 class="text-xl font-semibold text-white">Download the Launcher</h2>
            <p class="mt-2 text-sm text-slate-300">
              Self-contained Windows build with automatic patches, and expansion management.
            </p>

            <div v-if="downloadChips.length" class="mt-4 flex flex-wrap gap-2">
              <span
                v-for="chip in downloadChips"
                :key="chip.label"
                class="inline-flex items-center gap-2 rounded-full border border-white/10 bg-white/5 px-3 py-1 text-xs uppercase tracking-[0.2em] text-slate-200"
              >
                <span class="text-[0.65rem] text-brand-200">{{ chip.label }}</span>
                <span class="text-slate-100">{{ chip.value }}</span>
              </span>
            </div>

            <div v-if="launcherState.error" class="mt-4 rounded-2xl border border-red-500/40 bg-red-500/10 p-4 text-xs text-red-200">
              {{ launcherState.error }}
            </div>

            <div v-else class="mt-5 flex flex-wrap items-center gap-3">
              <a
                v-if="hasDownload"
                :href="downloadUrl"
                target="_blank"
                rel="noopener"
                class="inline-flex items-center gap-2 rounded-full bg-brand-500 px-5 py-2 text-sm font-semibold text-white shadow-lg transition hover:bg-brand-400"
              >
                <span>{{ downloadCtaText }}</span>
              </a>
            </div>

            <p class="mt-4 text-xs text-slate-400">
              Requires an existing purchased game installation.
            </p>

            <p v-if="releaseNotesPreview" class="mt-4 border-l-2 border-brand-500/50 pl-3 text-xs text-slate-300">
              {{ releaseNotesPreview }}
            </p>

          </div>
        </div>
      </div>
    </section>

    <section class="px-6 pb-20">
      <div class="mx-auto flex max-w-6xl flex-col gap-12 lg:flex-row">
        <div class="flex-1 space-y-6">
          <div class="max-w-2xl">
            <p class="text-sm font-semibold uppercase tracking-[0.3em] text-brand-200">Install OpenNova</p>
            <h2 class="mt-3 text-3xl font-semibold text-white sm:text-4xl">Fixes the multiplayer, upgrades the game.</h2>
            <p class="mt-4 text-base text-slate-300">
              No fussing, no manual patches—just launch, pick your expansion, and jump in.
            </p>
          </div>
          <div class="grid gap-4 md:grid-cols-2">
            <article
              v-for="feature in featureHighlights"
              :key="feature.title"
              class="rounded-3xl border border-white/5 bg-white/5 p-5"
            >
              <h3 class="text-base font-semibold text-white">{{ feature.title }}</h3>
              <p class="mt-2 text-sm text-slate-300">{{ feature.description }}</p>
            </article>
          </div>
        </div>

        <div class="flex-1 rounded-3xl border border-white/5 bg-gradient-to-br from-brand-500/10 via-slate-900 to-slate-950 p-7">
          <div class="flex items-center justify-between gap-3">
            <h3 class="text-xl font-semibold text-white">Live status</h3>
            <span v-if="statsUpdatedAtLabel" class="text-xs uppercase tracking-[0.2em] text-slate-400">{{ statsUpdatedAtLabel }}</span>
          </div>
          <p class="mt-2 text-sm text-slate-300">
            Live glimpse of games, lobbies, and players reporting through OpenNova.
          </p>

          <div v-if="statsLoading" class="mt-6 text-sm text-slate-400">Checking network status…</div>
          <div v-else-if="statsError" class="mt-6 rounded-2xl border border-red-500/40 bg-red-500/10 p-4 text-xs text-red-200">
            {{ statsError }}
          </div>
          <div v-else class="mt-6 grid gap-4 sm:grid-cols-3">
            <div class="rounded-2xl border border-white/5 bg-slate-950/70 p-4 text-center">
              <p class="text-[0.65rem] uppercase tracking-[0.3em] text-brand-200">Games</p>
              <p class="mt-2 text-2xl font-semibold text-white">{{ statsDisplay.games }}</p>
            </div>
            <div class="rounded-2xl border border-white/5 bg-slate-950/70 p-4 text-center">
              <p class="text-[0.65rem] uppercase tracking-[0.3em] text-brand-200">Lobbies</p>
              <p class="mt-2 text-2xl font-semibold text-white">{{ statsDisplay.lobbies }}</p>
            </div>
            <div class="rounded-2xl border border-white/5 bg-slate-950/70 p-4 text-center">
              <p class="text-[0.65rem] uppercase tracking-[0.3em] text-brand-200">Players</p>
              <p class="mt-2 text-2xl font-semibold text-white">{{ statsDisplay.players }}</p>
            </div>
          </div>
        </div>
      </div>
    </section>

    <section class="px-6 pb-24">
      <div class="mx-auto max-w-4xl rounded-3xl border border-white/5 bg-white/5 p-8 text-center">
        <h2 class="text-3xl font-semibold text-white">Ready to drop in?</h2>
        <p class="mt-3 text-base text-slate-300">
          Download the launcher, sign in with your OpenNova credentials, and we’ll handle the patches and expansions from there.
        </p>
        <div class="mt-6 flex flex-wrap justify-center gap-4">
          <a
            v-if="hasDownload"
            :href="downloadUrl"
            target="_blank"
            rel="noopener"
            class="inline-flex items-center gap-2 rounded-full bg-brand-500 px-6 py-3 font-semibold text-white shadow-lg transition hover:bg-brand-400"
          >
            {{ downloadCtaText }}
          </a>
          <RouterLink
            to="/expansions"
            class="inline-flex items-center gap-2 rounded-full border border-white/20 px-6 py-3 font-semibold text-white transition hover:border-brand-200"
          >
            Browse expansion catalog
          </RouterLink>
        </div>
      </div>
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

const launcherState = reactive<DownloadState>({
  loading: true,
  error: '',
  manifest: null,
});

const statsLoading = ref(true);
const statsError = ref('');
const stats = reactive({
  games: 0,
  lobbies: 0,
  players: 0,
  updatedAt: '',
});

const featureHighlights = [
  {
    title: 'Multiplayer restored',
    description: 'Joint Ops & DFX2 matchmaking all from the game client.',
  },
  {
    title: 'Automatic patches',
    description: 'Bigger maps, higher texture limits, and modern tick rate applied for you.',
  },
  {
    title: 'Expansion manager built-in',
    description: 'Grab vetted expansions from the curated catalog and swap releases in one click.',
  },
];


const launcherManifest = computed(() => launcherState.manifest);

const hasDownload = computed(() => !!launcherManifest.value?.downloadUrl);
const downloadUrl = computed(() => launcherManifest.value?.downloadUrl || '');

const platformLabel = computed(() => {
  const m = launcherManifest.value;
  if (!m) {
    return 'Windows';
  }

  const parts = [m.platform || 'Windows'];
  if (m.architecture) {
    parts.push(m.architecture);
  }
  return parts.join(' ');
});

const downloadCtaText = computed(() => `Download for ${platformLabel.value}`);
const releaseDateFormatted = computed(() => formatReleaseDate(launcherManifest.value?.releaseDate));

const downloadChips = computed(() => buildChips(launcherManifest.value, releaseDateFormatted.value));

function buildChips(manifest: DownloadManifest | null, releaseDate?: string) {
  if (!manifest) {
    return [] as Array<{ label: string; value: string }>;
  }

  const chips: Array<{ label: string; value: string }> = [];

  if (manifest.version) {
    chips.push({ label: 'Version', value: manifest.version });
  }

  if (releaseDate) {
    chips.push({ label: 'Released', value: releaseDate });
  }

  if (manifest.sizeHuman) {
    chips.push({ label: 'Size', value: manifest.sizeHuman });
  }

  return chips;
}

const launcherDescription = computed(() => {
  if (launcherManifest.value?.description) {
    return launcherManifest.value.description;
  }
  return 'Grab the OpenNova Launcher for quick updates, and seamless patches';
});

const releaseNotesPreview = computed(() => {
  const notes = launcherManifest.value?.releaseNotes?.trim();
  if (!notes) {
    return '';
  }
  return notes.length > 300 ? `${notes.slice(0, 300).trim()}…` : notes;
});

const statsDisplay = computed(() => ({
  games: stats.games.toLocaleString(),
  lobbies: stats.lobbies.toLocaleString(),
  players: stats.players.toLocaleString(),
}));

const statsUpdatedAtLabel = computed(() => {
  if (!stats.updatedAt) {
    return '';
  }

  const parsed = new Date(stats.updatedAt);
  if (Number.isNaN(parsed.getTime())) {
    return '';
  }

  return parsed.toLocaleTimeString();
});

onMounted(async () => {
  loadLauncherManifest();

  const statsController = new AbortController();
  try {
    const fetchedStats = await fetchStats(statsController.signal);
    stats.games = fetchedStats.games;
    stats.lobbies = fetchedStats.lobbies;
    stats.players = fetchedStats.players;
    stats.updatedAt = fetchedStats.updatedAt;
  } catch (error) {
    console.error('Failed to fetch stats', error);
    statsError.value = parseErrorMessage(error, 'Unable to load status information right now. Please try again later.');
  } finally {
    statsLoading.value = false;
  }
});

let launcherController: AbortController | null = null;

onUnmounted(() => {
  launcherController?.abort();
});

async function loadLauncherManifest() {
  launcherState.loading = true;
  launcherState.error = '';
  launcherController?.abort();
  launcherController = new AbortController();
  try {
    const fetched = await fetchLauncherManifest(launcherController.signal);
    launcherState.manifest = fetched;
  } catch (error) {
    console.error('Failed to fetch launcher manifest', error);
    launcherState.error = parseErrorMessage(error);
  } finally {
    launcherState.loading = false;
  }
}

function formatReleaseDate(value?: string) {
  if (!value) {
    return undefined;
  }
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) {
    return undefined;
  }
  return parsed.toLocaleDateString();
}

function parseErrorMessage(error: unknown, fallbackMessage = 'Unable to load launcher details right now. Please try again later.'): string {
  const defaultMessage = fallbackMessage;

  if (!error) {
    return defaultMessage;
  }

  const axiosError = error as AxiosError<{ error?: string; detail?: string }>;

  const detail = axiosError.response?.data?.detail || axiosError.response?.data?.error;
  if (detail) {
    return `${defaultMessage} (${detail})`;
  }

  if (axiosError.message) {
    return `${defaultMessage} (${axiosError.message})`;
  }

  return defaultMessage;
}
</script>
