<template>
  <section class="page space-y-6 py-10 sm:py-14">
    <PageHeader title="Asset library" refreshable :loading="loading" @refresh="load" />
    <p class="text-sm text-ink-muted">Shared native assets from published releases. Downloads include their dependencies.</p>

    <form class="flex flex-wrap items-end gap-3" aria-label="Find assets" @submit.prevent="search">
      <div class="min-w-48 flex-1">
        <label for="assets-search" class="field-label">Filename</label>
        <input id="assets-search" v-model="query" type="search" class="field" placeholder="Find an asset" />
      </div>
      <div>
        <label for="assets-kind" class="field-label">Kind</label>
        <select id="assets-kind" v-model="kind" class="field">
          <option value="">All kinds</option>
          <option v-for="option in ASSET_KINDS" :key="option.value" :value="option.value">{{ option.label }}</option>
        </select>
      </div>
      <div>
        <label for="assets-project" class="field-label">Project key</label>
        <input id="assets-project" v-model="project" type="text" class="field w-44 font-mono" placeholder="Any project" autocomplete="off" />
      </div>
      <button type="submit" class="btn-secondary">Search</button>
      <RouterLink v-if="filtered" to="/assets" class="btn-ghost">Clear</RouterLink>
    </form>

    <Notice v-if="error" tone="bad" role="alert">{{ error }}</Notice>
    <Notice v-if="downloadError" tone="bad" role="alert">{{ downloadError }}</Notice>

    <div class="table-wrap" :aria-busy="loading">
      <table class="data-table">
        <caption class="sr-only">Shared native assets and their downloadable bundles</caption>
        <thead>
          <tr>
            <th scope="col">Asset</th>
            <th scope="col">Kind</th>
            <th scope="col">Project</th>
            <th scope="col">Release</th>
            <th scope="col">Bundle</th>
            <th scope="col"><span class="sr-only">Download</span></th>
          </tr>
        </thead>
        <tbody>
          <TableState v-if="!data || data.assets.length === 0" :colspan="6" :rows="data?.assets ?? null" :loading="loading"
            :empty="filtered ? 'No assets match these filters.' : 'No shared assets yet.'" />
          <tr v-for="asset in data?.assets ?? []" :key="asset.id" class="asset-row">
            <th scope="row" class="break-words font-mono" :title="formatBytes(asset.size)">{{ asset.name }}</th>
            <td class="whitespace-nowrap">{{ kindLabel(asset.kind) }}</td>
            <td>
              <RouterLink :to="{ path: '/assets', query: { project: asset.project.key } }" class="entity-link">{{ asset.project.name }}</RouterLink>
              <span class="block font-mono text-xs text-ink-muted">{{ asset.project.key }}</span>
              <span v-if="asset.project.owner" class="block text-xs text-ink-muted">by {{ asset.project.owner }}</span>
            </td>
            <td>{{ asset.release.version || `Release ${asset.release.id}` }}</td>
            <td class="whitespace-nowrap" title="Files and unpacked size, including dependencies">
              {{ asset.file_count }} {{ asset.file_count === 1 ? 'file' : 'files' }} · {{ formatBytes(asset.total_bytes) }}
            </td>
            <td>
              <button type="button" class="btn-secondary btn-sm" :disabled="preparing.has(asset.id)"
                :aria-label="`${preparing.has(asset.id) ? 'Preparing' : 'Download'} ${asset.name}`" @click="download(asset)">
                <span :role="preparing.has(asset.id) ? 'status' : undefined">{{ preparing.has(asset.id) ? 'Preparing…' : 'Download' }}</span>
              </button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <nav v-if="before || data?.next_before" aria-label="Asset pages" class="flex items-center justify-between gap-3">
      <RouterLink v-if="before" :to="pageLink()" class="btn-secondary btn-sm">First page</RouterLink>
      <span v-else />
      <RouterLink v-if="data?.next_before" :to="pageLink(data.next_before)" class="btn-secondary btn-sm">Next</RouterLink>
    </nav>
  </section>
</template>

<script setup lang="ts">
import { computed, onUnmounted, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import Notice from '../components/ui/Notice.vue';
import PageHeader from '../components/ui/PageHeader.vue';
import TableState from '../components/ui/TableState.vue';
import { ASSET_KINDS, fetchAssets, isAssetKind, prepareAssetDownload } from '../api/assets';
import type { AssetFilters, AssetKind, AssetsPage, SharedAsset } from '../types/assets';
import { formatBytes } from '../utils/format';

const route = useRoute();
const router = useRouter();
const query = ref('');
const kind = ref<AssetKind | ''>('');
const project = ref('');
const data = ref<AssetsPage | null>(null);
const loading = ref(false);
const error = ref('');
const downloadError = ref('');
const preparing = ref(new Set<number>());
let listRequest: AbortController | null = null;
const downloads = new Map<number, AbortController>();
const timers = new Map<number, ReturnType<typeof setTimeout>>();

const textQuery = (key: string): string => typeof route.query[key] === 'string' ? route.query[key] as string : '';
const before = computed(() => {
  const value = Number(textQuery('before'));
  return Number.isSafeInteger(value) && value > 0 ? value : undefined;
});
const filters = computed<AssetFilters>(() => ({
  ...(textQuery('q').trim() ? { q: textQuery('q').trim() } : {}),
  ...(isAssetKind(textQuery('kind')) ? { kind: textQuery('kind') as AssetKind } : {}),
  ...(textQuery('project').trim() ? { project: textQuery('project').trim() } : {}),
  ...(before.value ? { before: before.value } : {}),
}));
const filtered = computed(() => Boolean(filters.value.q || filters.value.kind || filters.value.project));

function kindLabel(value: AssetKind): string {
  return ASSET_KINDS.find((option) => option.value === value)!.label;
}

function pageLink(cursor?: number) {
  return { path: '/assets', query: { ...route.query, before: cursor ? String(cursor) : undefined } };
}

async function search() {
  const next = { q: query.value.trim() || undefined, kind: kind.value || undefined, project: project.value.trim() || undefined };
  if (router.resolve({ path: '/assets', query: next }).fullPath === route.fullPath) await load();
  else await router.push({ path: '/assets', query: next });
}

async function load() {
  listRequest?.abort();
  const controller = new AbortController();
  listRequest = controller;
  loading.value = true;
  error.value = '';
  const result = await fetchAssets(filters.value, controller.signal);
  if (controller.signal.aborted) return;
  loading.value = false;
  if (result.ok) data.value = result.value;
  else error.value = result.error;
}

function stopDownloads() {
  for (const controller of downloads.values()) controller.abort();
  for (const timer of timers.values()) clearTimeout(timer);
  downloads.clear();
  timers.clear();
  preparing.value.clear();
}

function download(asset: SharedAsset) {
  if (preparing.value.has(asset.id)) return;
  downloadError.value = '';
  const controller = new AbortController();
  downloads.set(asset.id, controller);
  preparing.value.add(asset.id);
  async function attempt(retry = false) {
    const result = await prepareAssetDownload(asset.id, controller.signal, retry);
    if (controller.signal.aborted) return;
    if (result.ok && result.value.state === 'preparing') {
      timers.set(asset.id, setTimeout(() => { void attempt(); }, 2000));
      return;
    }
    downloads.delete(asset.id);
    timers.delete(asset.id);
    preparing.value.delete(asset.id);
    if (!result.ok) {
      downloadError.value = `${asset.name}: ${result.error}`;
      return;
    }
    if (result.value.state === 'ready') {
      const link = document.createElement('a');
      link.href = result.value.url;
      link.download = result.value.filename;
      link.rel = 'noopener';
      document.body.append(link);
      link.click();
      link.remove();
    }
  }
  void attempt(true);
}

watch(filters, () => {
  query.value = filters.value.q ?? '';
  kind.value = filters.value.kind ?? '';
  project.value = filters.value.project ?? '';
  data.value = null;
  stopDownloads();
  downloadError.value = '';
  void load();
}, { immediate: true });

onUnmounted(() => {
  listRequest?.abort();
  stopDownloads();
});
</script>
