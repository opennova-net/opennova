<template>
  <section class="page space-y-8 py-10 sm:py-14">
    <PageHeader title="Downloads">
      <template #meta>
        <span>Windows x64, pre-1.0 and experimental.</span>
        <span v-if="manifest" data-test="downloads-written">Updated <time :datetime="isoTime(manifest.written)">{{ formatUtc(manifest.written) }}</time></span>
        <RouterLink to="/play" class="link">Which one?</RouterLink>
      </template>
    </PageHeader>

    <p v-if="!manifest && loading" role="status" class="text-sm text-ink-muted">Checking downloads…</p>
    <Notice v-else-if="!manifest && downloadsHost" tone="warn" role="status">The downloads could not be read. Refresh to try again.</Notice>
    <p v-else-if="!manifest" class="text-sm text-ink-muted">Downloads are not configured for this site.</p>

    <template v-else>
      <section v-for="group in shownGroups" :key="group.id" class="space-y-4" :aria-labelledby="group.id">
        <div class="flex flex-wrap items-baseline gap-x-3">
          <h2 :id="group.id" class="section-heading scroll-mt-24">{{ group.title }}</h2>
          <span v-if="group.note" class="text-xs text-ink-muted">{{ group.note }}</span>
        </div>
        <ul class="grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
          <li v-for="item in group.products" :id="anchorOf(item)" :key="item.key" class="card scroll-mt-24 flex flex-col gap-3 p-5">
            <div>
              <h3 class="font-semibold text-ink-bright">{{ item.name }}</h3>
              <p v-if="item.owner" class="text-sm text-ink-muted" data-test="owner">by {{ item.owner }}</p>
              <p v-if="facts(item)" class="text-sm text-ink-muted" data-test="facts">{{ facts(item) }}</p>
            </div>
            <a v-if="downloadUrl(item)" :href="downloadUrl(item)" class="btn-primary self-start" :aria-label="'Download ' + item.name">Download</a>
            <span v-else class="btn-secondary self-start cursor-not-allowed opacity-50" aria-disabled="true">Not published yet</span>
            <details v-if="item.file" class="text-xs text-ink-muted" :data-test="'build-' + item.file">
              <summary>Details</summary>
              <dl class="mt-2 grid grid-cols-[auto_1fr] gap-x-3 gap-y-1 break-all font-mono">
                <dt>File</dt>
                <dd>{{ item.file }}</dd>
                <template v-if="item.sha256">
                  <dt>SHA-256</dt>
                  <dd>{{ item.sha256 }}</dd>
                </template>
                <template v-if="item.published">
                  <dt>Published</dt>
                  <dd><time :datetime="isoTime(item.published)">{{ formatUtc(item.published) }}</time></dd>
                </template>
              </dl>
            </details>
          </li>
        </ul>
      </section>
    </template>
  </section>
</template>

<script setup lang="ts">
// /downloads: this deployment's downloads host (downloads.<the site's
// hostname>) and what its manifest says of each product: the games (and an
// expansion with a download of its own), the apps, then the games players
// published (the manifest names their owner). A product with no file (the
// runtime, an expansion its game's zip installs) is not shown; a site with no
// host (a local stack) offers no download.
import { computed, onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import Notice from '../components/ui/Notice.vue';
import PageHeader from '../components/ui/PageHeader.vue';
import { downloadsBase, fetchDownloadsManifest, type DownloadsManifest, type DownloadsProduct } from '../api/downloads';
import { formatBytes, formatUtc, isoTime } from '../utils/format';

const downloadsHost = downloadsBase(window.location.hostname);
const manifest = ref<DownloadsManifest | null>(null);
const loading = ref(Boolean(downloadsHost));

const shownGroups = computed(() => {
  const offered = (manifest.value?.products ?? []).filter((product) => product.file);
  const players = offered.filter(isPlayersGame);
  const ours = offered.filter((product) => !isPlayersGame(product));
  return [
    { id: 'games', title: 'Games', note: '', products: ours.filter((product) => product.kind !== 'app') },
    { id: 'apps', title: 'Apps', note: '', products: ours.filter((product) => product.kind === 'app') },
    { id: 'players-games', title: "Players' games", note: 'Published by players, not reviewed by OpenNova.', products: players },
  ].filter((group) => group.products.length > 0);
});

function isPlayersGame(product: DownloadsProduct): boolean {
  return product.kind === 'game' && typeof product.owner === 'string' && product.owner !== '';
}

// A game's card answers to its zip's name ("#jointops" for jointops-game-windows.zip, as the Play
// page links it; "#player-rangers" for a player's player-rangers-game-windows.zip); any other
// product's to its key.
function anchorOf(product: DownloadsProduct): string {
  const suffix = '-game-windows.zip';
  return product.file?.endsWith(suffix) ? product.file.slice(0, -suffix.length) : product.key.toLowerCase();
}

// The product's file on this deployment's host once it has a published release; none before.
function downloadUrl(product: DownloadsProduct): string | undefined {
  return downloadsHost && product.file && product.sha256 ? `${downloadsHost}/${product.file}` : undefined;
}

// "v0.0.22 · 53 MB": the published release's version and its file's size.
function facts(product: DownloadsProduct): string {
  if (!product.sha256) return '';
  const size = product.size === null ? '' : formatBytes(product.size);
  return [product.version ? `v${product.version}` : '', size].filter(Boolean).join(' · ');
}

onMounted(async () => {
  if (!downloadsHost) return;
  try {
    manifest.value = await fetchDownloadsManifest(downloadsHost);
  } catch {
    manifest.value = null;
  } finally {
    loading.value = false;
  }
});
</script>
