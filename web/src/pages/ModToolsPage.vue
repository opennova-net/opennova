<template>
  <main class="space-y-24 bg-surface">
    <section class="px-6 pt-24 pb-24">
      <div class="mx-auto max-w-4xl text-center">
        <p class="text-sm uppercase tracking-[0.3em] text-accent">Game releases</p>
        <h1 class="mt-4 text-4xl font-bold text-ink sm:text-5xl">OpenNova downloads</h1>
        <p class="mt-6 max-w-3xl mx-auto text-lg text-ink-muted">
          OpenNova keeps game data as ordinary source-controlled files. ONED is the compact
          companion for selecting loose or packed game data, running OpenNova, staging and
          running retail for comparison, and stopping the process it started. Runtime game
          data stays in ordinary source-controlled files.
        </p>
        <p class="mt-4 max-w-3xl mx-auto text-lg text-ink-muted">
          Everything is pre-1.0, open source, and under active development. Tagged releases
          publish the Windows game. CI development builds pair the game, ONED, and the loose
          sources. Start with the README and docs for current workflows and known limits.
        </p>
      </div>
    </section>

    <section class="px-6 pb-24">
      <div class="mx-auto max-w-4xl">
        <!-- Loading -->
        <p v-if="state.loading" class="text-center text-sm text-ink-muted">
          Loading the latest release…
        </p>

        <!-- Error: never a dead end — fall back to the releases page. -->
        <div v-else-if="state.error" class="text-center">
          <p class="text-sm text-danger">{{ state.error }}</p>
          <a
            :href="releasesPageUrl"
            target="_blank"
            rel="noopener"
            class="mt-6 inline-flex items-center gap-2 rounded-control bg-accent px-6 py-3 font-semibold text-on-accent transition hover:bg-accent/90"
          >
            Browse releases on GitHub
          </a>
        </div>

        <!-- Downloads -->
        <template v-else-if="release">
          <div class="flex flex-col items-center text-center">
            <p class="text-xs uppercase tracking-[0.3em] text-accent">Latest release</p>
            <h2 class="mt-2 text-2xl font-bold text-ink">Windows download</h2>
            <p class="mt-1 text-sm text-ink-muted">Release v{{ release.version }}</p>
          </div>

          <div class="mt-8 grid gap-4 sm:grid-cols-2">
            <a
              v-for="asset in release.assets"
              :key="asset.filename"
              :href="asset.downloadUrl"
              class="group flex flex-col rounded-panel border border-border-strong bg-raised p-5 transition hover:border-accent"
            >
              <span class="text-base font-semibold text-ink group-hover:text-ink-bright">
                {{ asset.product }}
              </span>
              <span class="mt-1 text-sm text-accent">Windows · Download</span>
              <span class="mt-3 truncate text-xs text-ink-muted">
                {{ asset.filename }}<template v-if="asset.sizeHuman"> · {{ asset.sizeHuman }}</template>
              </span>
            </a>
          </div>

        </template>

        <div class="mt-12 flex flex-wrap items-center justify-center gap-4">
          <a
            :href="releasesPageUrl"
            target="_blank"
            rel="noopener"
            class="inline-flex items-center gap-2 rounded-control border border-border-strong px-6 py-3 font-semibold text-ink transition hover:border-accent hover:text-ink-bright"
          >
            All releases
          </a>
          <a
            href="https://github.com/opennova-net/opennova"
            target="_blank"
            rel="noopener"
            class="inline-flex items-center gap-2 rounded-control border border-border-strong px-6 py-3 font-semibold text-ink transition hover:border-accent hover:text-ink-bright"
          >
            View on GitHub
          </a>
        </div>
      </div>
    </section>
  </main>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, reactive } from 'vue';
import type { AxiosError } from 'axios';
import { fetchLatestGameRelease } from '../api/releases';
import type { GameRelease } from '../types/downloads';

const releasesPageUrl = 'https://github.com/opennova-net/opennova/releases';

const state = reactive<{ loading: boolean; error: string; release: GameRelease | null }>({
  loading: true,
  error: '',
  release: null,
});

const release = computed(() => state.release);

let controller: AbortController | null = null;

onMounted(async () => {
  controller = new AbortController();
  try {
    state.release = await fetchLatestGameRelease(controller.signal);
  } catch (error) {
    console.error('Failed to fetch game release', error);
    state.error = parseErrorMessage(error);
  } finally {
    state.loading = false;
  }
});

onUnmounted(() => {
  controller?.abort();
});

function parseErrorMessage(error: unknown, fallback = 'Unable to load the latest downloads right now.'): string {
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
