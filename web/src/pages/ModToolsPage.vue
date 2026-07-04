<template>
  <main class="space-y-24 bg-surface">
    <section class="px-6 pt-24 pb-24">
      <div class="mx-auto max-w-4xl text-center">
        <p class="text-sm uppercase tracking-[0.3em] text-accent">Community tools</p>
        <h1 class="mt-4 text-4xl font-bold text-ink sm:text-5xl">Tools</h1>
        <p class="mt-6 max-w-3xl mx-auto text-lg text-ink-muted">
          OpenNova is built around a portable C++ core that reads and writes the original
          NovaLogic formats directly: terrain, 3DI models, missions, menus, audio, and the
          PFF archives that hold them. The OpenNova Editor (ONED) puts that core to work
          with workspaces for terrain painting, mission authoring, environment setup, and
          asset browsing, so you can open a retail install of Joint Operations or a newer
          title and start editing what shipped.
        </p>
        <p class="mt-4 max-w-3xl mx-auto text-lg text-ink-muted">
          Everything is pre-1.0, open source, and under active development. Editor and
          runtime builds for Windows and macOS are published from CI on every release.
          The same repository carries the Blender and 3ds Max exchange tools and the
          Python utilities used to take assets apart and put them back together. Start
          with the README and the docs folder for current workflows and known limits.
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
            <h2 class="mt-2 text-2xl font-bold text-ink">Downloads for {{ osLabel }}</h2>
            <p class="mt-1 text-sm text-ink-muted">Release v{{ release.version }}</p>
          </div>

          <div class="mt-8 grid gap-4 sm:grid-cols-2">
            <a
              v-for="asset in primaryAssets"
              :key="asset.filename"
              :href="asset.downloadUrl"
              class="group flex flex-col rounded-panel border border-border-strong bg-raised p-5 transition hover:border-accent"
            >
              <span class="text-base font-semibold text-ink group-hover:text-ink-bright">
                {{ asset.product }}
              </span>
              <span class="mt-1 text-sm text-accent">{{ osName(asset.os) }} · Download</span>
              <span class="mt-3 truncate text-xs text-ink-muted">
                {{ asset.filename }}<template v-if="asset.sizeHuman"> · {{ asset.sizeHuman }}</template>
              </span>
            </a>
          </div>

          <div v-if="otherAssets.length" class="mt-12">
            <h3 class="text-sm font-semibold uppercase tracking-wide text-ink-muted">
              Other platforms
            </h3>
            <div class="mt-4 grid gap-3 sm:grid-cols-2">
              <a
                v-for="asset in otherAssets"
                :key="asset.filename"
                :href="asset.downloadUrl"
                class="flex items-center justify-between gap-3 rounded-control border border-border-strong px-4 py-3 text-sm transition hover:border-accent"
              >
                <span class="text-ink">{{ asset.product }} · {{ osName(asset.os) }}</span>
                <span v-if="asset.sizeHuman" class="shrink-0 text-xs text-ink-muted">
                  {{ asset.sizeHuman }}
                </span>
              </a>
            </div>
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
import { detectOs, fetchLatestToolRelease } from '../api/releases';
import type { ToolOs, ToolRelease } from '../types/downloads';

const releasesPageUrl = 'https://github.com/opennova-net/opennova/releases';

const state = reactive<{ loading: boolean; error: string; release: ToolRelease | null }>({
  loading: true,
  error: '',
  release: null,
});

const os = detectOs();
const release = computed(() => state.release);

// The visitor's OS build plus cross-platform ('any') downloads lead; the other OS follows.
const primaryAssets = computed(() =>
  (release.value?.assets ?? []).filter((a) => a.os === os || a.os === 'any'),
);
const otherAssets = computed(() =>
  (release.value?.assets ?? []).filter((a) => a.os !== os && a.os !== 'any'),
);

const osLabel = computed(() => osName(os));

function osName(value: ToolOs): string {
  if (value === 'macos') {
    return 'macOS';
  }
  if (value === 'windows') {
    return 'Windows';
  }
  return 'All platforms';
}

let controller: AbortController | null = null;

onMounted(async () => {
  controller = new AbortController();
  try {
    state.release = await fetchLatestToolRelease(controller.signal);
  } catch (error) {
    console.error('Failed to fetch tool release', error);
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
