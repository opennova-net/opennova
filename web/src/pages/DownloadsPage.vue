<template>
  <section class="page py-10 sm:py-14" aria-labelledby="downloads-title">
    <header class="max-w-3xl">
      <p class="eyebrow">Windows x64 · Pre-1.0</p>
      <h1 id="downloads-title" class="mt-3 text-4xl font-semibold tracking-tight text-ink-bright">Download OpenNova</h1>
      <p class="mt-4 leading-relaxed text-ink-muted">
        The games, the editor and the tools, all pre-1.0 and experimental. Unzip a game build into an empty folder
        and run <code>opennova.exe</code>; its first launch downloads the game. Joint Operations needs your own
        purchased copy.
      </p>
      <p class="mt-4 text-sm text-ink-muted">
        Not sure which one? <RouterLink to="/play" class="link">See how to set up each game</RouterLink>.
      </p>
    </header>

    <div class="mt-7 rounded-panel border border-border bg-panel p-4 text-sm text-ink-muted">
      <p v-if="manifest" data-test="downloads-written">
        Each download is its product's published release. Updated
        <time :datetime="isoTime(manifest.written)">{{ formatUtc(manifest.written) }}</time>.
      </p>
      <p v-else-if="loading" role="status">Checking available downloads...</p>
      <p v-else-if="downloadsHost" role="status" class="text-warn">The downloads manifest could not be loaded. Refresh this page to try again.</p>
      <p v-else>Downloads are not configured for this site.</p>
    </div>

    <template v-if="manifest">
      <template v-for="group in groups" :key="group.id">
        <h2 v-if="group.products.length" :id="group.id" class="mt-10 scroll-mt-24 text-xl font-semibold text-ink-bright">{{ group.title }}</h2>
        <ul v-if="group.products.length" class="mt-4 grid gap-4" :class="group.grid">
          <li v-for="item in group.products" :id="anchorOf(item)" :key="item.key" class="card scroll-mt-24 flex flex-col p-6">
            <h3 class="text-lg font-semibold text-ink-bright">{{ item.name }}</h3>
            <p class="mt-2 flex-1 text-sm leading-relaxed text-ink-muted">{{ item.description }}</p>
            <div class="mt-6 space-y-3">
              <a v-if="downloadUrl(item)" :href="downloadUrl(item)" class="btn-primary" :aria-label="'Download ' + item.name">Download</a>
              <span v-else class="btn-primary cursor-not-allowed opacity-50" aria-disabled="true">Not published yet</span>
              <div class="min-w-0 break-words font-mono text-xs text-ink-muted" :data-test="'build-' + item.file">
                <p>{{ item.file }}</p>
                <p v-if="item.sha256">{{ details(item) }}, SHA-256 <span :title="item.sha256">{{ item.sha256.slice(0, 12) }}</span></p>
                <p v-if="item.sha256 && item.published">
                  Published <time :datetime="isoTime(item.published)">{{ formatUtc(item.published) }}</time>
                </p>
              </div>
            </div>
          </li>
        </ul>
      </template>
    </template>
  </section>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import { downloadsBase, fetchDownloadsManifest, type DownloadsManifest, type DownloadsProduct } from '../api/downloads';
import { formatBytes, formatUtc, isoTime } from '../utils/format';

// This deployment's downloads host (downloads.<the site's hostname>), and what its manifest says
// of each product. A site with no host (a local stack) offers no download.
const downloadsHost = downloadsBase(window.location.hostname);
const manifest = ref<DownloadsManifest | null>(null);
const loading = ref(Boolean(downloadsHost));

// The products the host has a file for, in the manifest's order: the games (and an expansion
// with a download of its own) first, then the apps. A product with no file (the runtime, an
// expansion its game's zip installs) is not shown.
const groups = computed(() => {
  const offered = (manifest.value?.products ?? []).filter((product) => product.file);
  return [
    { id: 'games', title: 'Games', grid: 'lg:grid-cols-3', products: offered.filter((product) => product.kind !== 'app') },
    { id: 'apps', title: 'Apps', grid: 'sm:grid-cols-2 lg:grid-cols-3', products: offered.filter((product) => product.kind === 'app') },
  ];
});

// A game's card answers to its zip's name ("#jointops" for jointops-game-windows.zip, as the Play
// page links it); any other product's to its key.
function anchorOf(product: DownloadsProduct): string {
  const suffix = '-game-windows.zip';
  return product.file?.endsWith(suffix) ? product.file.slice(0, -suffix.length) : product.key.toLowerCase();
}

// The product's file on this deployment's host once it has a published release; none before.
function downloadUrl(product: DownloadsProduct): string | undefined {
  return downloadsHost && product.file && product.sha256 ? `${downloadsHost}/${product.file}` : undefined;
}

// "v0.0.22, 53 MB": the published release's version and its file's size.
function details(product: DownloadsProduct): string {
  const size = product.size === null ? '' : formatBytes(product.size);
  return [product.version ? `v${product.version}` : '', size].filter(Boolean).join(', ');
}

onMounted(async () => {
  if (!downloadsHost) return;
  try { manifest.value = await fetchDownloadsManifest(downloadsHost); }
  catch { manifest.value = null; }
  finally { loading.value = false; }
});
</script>
