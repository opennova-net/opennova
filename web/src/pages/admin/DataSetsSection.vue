<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-start sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Game data</h1>
        <p class="text-sm text-ink-muted">
          The data hosted servers run: each data set is one product, a base game or one expansion of it, with
          the files you upload here (a tree). New servers start on a data set's current tree; a running server
          keeps the tree it started on until its owner updates it, or you update every outdated one.
        </p>
        <p v-if="data" class="mt-1 text-xs text-ink-muted">
          Files go from this browser through the site to {{ data.store.kind === 's3' ? 'S3' : "the service's local store" }},
          in {{ formatMemory(PART_SIZE) }} parts.
        </p>
      </div>
      <div class="flex items-center gap-3">
        <p v-if="loadedAt" class="text-xs text-ink-muted">Updated <RelativeTime :value="loadedAt" :now="now" /></p>
        <button type="button" :class="secondaryButton" :disabled="poll.loading.value" @click="poll.refresh">
          {{ poll.loading.value ? 'Refreshing…' : 'Refresh' }}
        </button>
      </div>
    </header>

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them; the boxes below are their visible copies. -->
    <div role="status" class="sr-only">{{ message }}</div>
    <div role="alert" class="sr-only">{{ error }}</div>
    <div v-if="message" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">{{ message }}</div>
    <div v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">{{ error }}</div>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>

    <!-- The data sets. -->
    <section class="space-y-4" aria-labelledby="ds-heading">
      <div class="flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
        <h2 id="ds-heading" class="text-xl font-semibold text-ink">Data sets</h2>
        <button id="ds-new-toggle" type="button" :class="secondaryButton" :disabled="!data" @click="toggleNewForm">
          {{ creating ? 'Close the form' : 'New data set…' }}
        </button>
      </div>

      <form v-if="creating && data" id="ds-new-form" class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6" novalidate @submit.prevent="create">
        <h3 class="text-lg font-semibold text-ink">New data set</h3>
        <div class="grid gap-4 sm:grid-cols-2">
          <label class="space-y-1">
            <span :class="labelClass">Kind</span>
            <select id="ds-new-kind" v-model="newForm.kind" :class="inputClass">
              <option value="base">Base game</option>
              <option value="expansion">Expansion</option>
            </select>
          </label>
          <div class="space-y-1">
            <label :for="'ds-new-display_name'" :class="labelClass">Display name</label>
            <input id="ds-new-display_name" v-model="newForm.display_name" type="text" autocomplete="off" :class="inputClass" :aria-invalid="newErrors.display_name ? 'true' : undefined" />
            <p v-if="newErrors.display_name" id="ds-new-display_name-error" class="text-xs text-danger">{{ newErrors.display_name }}</p>
          </div>
          <template v-if="newForm.kind === 'base'">
            <div class="space-y-1">
              <label for="ds-new-title" :class="labelClass">Title</label>
              <select id="ds-new-title" v-model="newForm.title" :class="inputClass">
                <option v-for="t in data.titles" :key="t" :value="t">{{ titleName(t) }} ({{ t }})</option>
              </select>
              <p class="text-xs text-ink-muted">The game the server speaks: its protocol, its limits.</p>
              <p v-if="newErrors.title" id="ds-new-title-error" class="text-xs text-danger">{{ newErrors.title }}</p>
            </div>
            <div class="space-y-1">
              <label for="ds-new-game" :class="labelClass">Lobby</label>
              <select id="ds-new-game" v-model="newForm.game" :class="inputClass">
                <option v-for="g in data.games" :key="g.slug" :value="g.slug">{{ g.display_name }} ({{ g.slug }})</option>
              </select>
              <p class="text-xs text-ink-muted">The server browser its servers list in.</p>
              <p v-if="newErrors.game" id="ds-new-game-error" class="text-xs text-danger">{{ newErrors.game }}</p>
            </div>
          </template>
          <template v-else>
            <div class="space-y-1">
              <label for="ds-new-parent_id" :class="labelClass">Base game</label>
              <select id="ds-new-parent_id" v-model.number="newForm.parent_id" :class="inputClass">
                <option v-for="b in bases" :key="b.id" :value="b.id">{{ b.display_name }}</option>
              </select>
              <p v-if="newErrors.parent_id" id="ds-new-parent_id-error" class="text-xs text-danger">{{ newErrors.parent_id }}</p>
            </div>
            <div class="space-y-1">
              <label for="ds-new-expansion_code" :class="labelClass">Code</label>
              <input id="ds-new-expansion_code" v-model="newForm.expansion_code" type="text" autocomplete="off" autocapitalize="none" spellcheck="false" placeholder="jox01" maxlength="8" :class="[inputClass, 'font-mono']" :aria-invalid="newErrors.expansion_code ? 'true' : undefined" />
              <p class="text-xs text-ink-muted">The /exp code: its folder under expansion/, and its two archives' name.</p>
              <p v-if="newErrors.expansion_code" id="ds-new-expansion_code-error" class="text-xs text-danger">{{ newErrors.expansion_code }}</p>
            </div>
          </template>
        </div>
        <div class="flex flex-wrap gap-2">
          <button id="ds-new-submit" type="submit" :disabled="busy" :class="primaryButton">Create</button>
          <button type="button" :disabled="busy" :class="secondaryButton" @click="creating = false">Cancel</button>
        </div>
      </form>

      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Data sets</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">Data set</th>
              <th scope="col" class="px-4 py-3">Current data</th>
              <th scope="col" class="px-4 py-3">Servers</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="data === null">
              <td colspan="4" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="data.data_sets.length === 0">
              <td colspan="4" class="px-4 py-6 text-center text-ink-muted">No data set yet. Create one, then upload its files.</td>
            </tr>
            <template v-for="d in data?.data_sets ?? []" :key="d.id">
              <tr class="ds-row align-top hover:bg-hover">
                <th scope="row" class="px-4 py-3 text-left font-medium">
                  {{ d.display_name }}
                  <span class="block text-xs font-normal text-ink-muted">{{ kindLine(d) }}</span>
                </th>
                <td class="px-4 py-3">
                  <template v-if="d.current_tree">
                    <span class="block font-mono">{{ d.current_tree.version || `tree ${d.current_tree.id}` }}</span>
                    <span v-if="d.current_tree.ready_at" class="block text-xs text-ink-muted">
                      ready <RelativeTime :value="d.current_tree.ready_at" :now="now" />
                    </span>
                  </template>
                  <span v-else class="text-warn">No data yet</span>
                </td>
                <td class="whitespace-nowrap px-4 py-3">
                  {{ d.servers_live }} live
                  <span v-if="d.servers_outdated > 0" class="block text-xs text-warn">{{ d.servers_outdated }} outdated</span>
                </td>
                <td class="space-x-3 whitespace-nowrap px-4 py-3 text-right text-xs">
                  <button type="button" :disabled="busy || uploading" class="text-accent hover:text-ink-bright disabled:opacity-40" @click="openUploader(d)">
                    Upload data<span class="sr-only"> for {{ d.display_name }}</span>
                  </button>
                  <button
                    v-if="d.servers_outdated > 0"
                    type="button"
                    :disabled="busy"
                    class="text-warn/90 hover:text-warn disabled:opacity-40"
                    @click="updateServers(d)"
                  >
                    Update servers<span class="sr-only"> of {{ d.display_name }}</span>
                  </button>
                  <button type="button" :disabled="busy" class="text-accent hover:text-ink-bright disabled:opacity-40" @click="toggleEdit(d)">
                    {{ editing?.id === d.id ? 'Close' : 'Edit' }}<span class="sr-only"> {{ d.display_name }}</span>
                  </button>
                  <button type="button" class="text-accent hover:text-ink-bright" :aria-expanded="expanded.has(d.id) ? 'true' : 'false'" @click="toggleTrees(d)">
                    Trees ({{ d.trees.length }})<span class="sr-only"> of {{ d.display_name }}</span>
                  </button>
                  <button type="button" :disabled="busy" class="text-danger/80 hover:text-danger disabled:opacity-40" @click="removeDataSet(d)">
                    Delete<span class="sr-only"> {{ d.display_name }}</span>
                  </button>
                </td>
              </tr>
              <tr v-if="editing?.id === d.id" class="ds-edit-row bg-surface">
                <td colspan="4" class="px-4 py-4">
                  <form class="space-y-3" novalidate @submit.prevent="saveEdit">
                    <div class="grid gap-3 sm:grid-cols-2">
                      <label class="space-y-1">
                        <span :class="labelClass">Display name</span>
                        <input :id="`ds-edit-display_name-${d.id}`" v-model="editing.display_name" type="text" autocomplete="off" :class="inputClass" />
                      </label>
                      <label v-if="d.kind === 'base'" class="space-y-1">
                        <span :class="labelClass">Lobby</span>
                        <select :id="`ds-edit-game-${d.id}`" v-model="editing.game" :class="inputClass">
                          <option v-for="g in data?.games ?? []" :key="g.slug" :value="g.slug">{{ g.display_name }}</option>
                        </select>
                      </label>
                    </div>
                    <p v-if="editError" class="text-xs text-danger">{{ editError }}</p>
                    <div class="flex flex-wrap gap-2">
                      <button type="submit" :disabled="busy" :class="primaryButton">Save</button>
                      <button type="button" :disabled="busy" :class="secondaryButton" @click="editing = null">Cancel</button>
                    </div>
                  </form>
                </td>
              </tr>
              <tr v-if="expanded.has(d.id)" class="ds-trees-row bg-surface">
                <td colspan="4" class="px-4 py-3">
                  <p v-if="d.trees.length === 0" class="text-sm text-ink-muted">No tree yet: upload the data set's files.</p>
                  <table v-else class="min-w-full text-xs">
                    <caption class="sr-only">Trees of {{ d.display_name }}</caption>
                    <thead>
                      <tr class="text-left uppercase tracking-wider text-ink-muted">
                        <th scope="col" class="px-2 py-2">Tree</th>
                        <th scope="col" class="px-2 py-2">State</th>
                        <th scope="col" class="px-2 py-2">Version</th>
                        <th scope="col" class="px-2 py-2">Size</th>
                        <th scope="col" class="px-2 py-2">Files</th>
                        <th scope="col" class="px-2 py-2">Missions</th>
                        <th scope="col" class="px-2 py-2">Servers on it</th>
                        <th scope="col" class="px-2 py-2">Created</th>
                        <th scope="col" class="px-2 py-2 text-right">Actions</th>
                      </tr>
                    </thead>
                    <tbody class="divide-y divide-border text-ink">
                      <tr v-for="t in d.trees" :key="t.id" class="ds-tree-row align-top">
                        <td class="whitespace-nowrap px-2 py-2">
                          <span class="font-mono">{{ t.id }}</span>
                          <span v-if="d.current_tree?.id === t.id" class="ml-1 inline-block rounded-control bg-online/10 px-2 py-0.5 text-online">current</span>
                        </td>
                        <td class="px-2 py-2">
                          <span :class="treeStateClass(t.state)">{{ treeStateLabel(t.state) }}</span>
                          <span v-if="t.error" class="block max-w-xs whitespace-normal text-danger">{{ t.error }}</span>
                        </td>
                        <td class="whitespace-nowrap px-2 py-2 font-mono">{{ t.version || '-' }}</td>
                        <td class="whitespace-nowrap px-2 py-2">{{ formatBytes(t.bytes) }}</td>
                        <td class="whitespace-nowrap px-2 py-2">{{ t.files }}</td>
                        <td class="whitespace-nowrap px-2 py-2">{{ t.missions }}</td>
                        <td class="whitespace-nowrap px-2 py-2">{{ t.pinned_by }}</td>
                        <td class="whitespace-nowrap px-2 py-2 text-ink-muted">
                          <RelativeTime :value="t.created_at" :now="now" />
                          <span class="block">{{ creatorLabel(t.created_by) }}</span>
                        </td>
                        <td class="whitespace-nowrap px-2 py-2 text-right">
                          <button
                            v-if="t.state !== 'pruned'"
                            type="button"
                            :disabled="busy"
                            class="text-danger/80 hover:text-danger disabled:opacity-40"
                            @click="removeTree(d, t)"
                          >
                            Delete<span class="sr-only"> tree {{ t.id }}</span>
                          </button>
                        </td>
                      </tr>
                    </tbody>
                  </table>
                </td>
              </tr>
            </template>
          </tbody>
        </table>
      </div>
    </section>

    <!-- The uploader: a data set's files, through the site to the store, then
         the worker's finalize. -->
    <section
      v-if="uploader"
      id="ds-uploader"
      class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6"
      aria-labelledby="ds-uploader-heading"
    >
      <div class="flex items-start justify-between gap-4">
        <div>
          <p class="text-xs uppercase tracking-[0.3em] text-ink-muted">Upload data</p>
          <h2 id="ds-uploader-heading" tabindex="-1" class="mt-1 text-xl font-semibold text-ink focus:outline-none">{{ uploader.dataSet.display_name }}</h2>
        </div>
        <button v-if="!uploading" type="button" class="shrink-0 text-sm text-ink-muted hover:text-ink" @click="closeUploader">Close</button>
      </div>
      <p class="text-sm text-ink-muted">
        <template v-if="uploader.dataSet.kind === 'base'">
          Pick the game's install folder. Only the files a server needs are taken from its root:
          {{ ruleList(uploader.dataSet) }}. Nothing else is uploaded.
        </template>
        <template v-else>
          Pick the expansion's own folder (expansion/{{ uploader.dataSet.expansion_code }} in the game's install). Only
          {{ ruleList(uploader.dataSet) }} are taken. Nothing else is uploaded.
        </template>
      </p>

      <template v-if="uploader.phase === 'pick'">
        <div class="flex flex-wrap items-end gap-4">
          <label class="space-y-1">
            <span :class="labelClass">Folder</span>
            <input id="ds-upload-folder" type="file" webkitdirectory directory multiple class="block text-sm text-ink" @change="onPicked" />
          </label>
          <label class="space-y-1">
            <span :class="labelClass">Or the files themselves</span>
            <input id="ds-upload-files" type="file" multiple class="block text-sm text-ink" @change="onPicked" />
          </label>
        </div>
        <div v-if="uploader.picked" id="ds-upload-checklist" class="space-y-2">
          <ul class="divide-y divide-border rounded-panel border border-border text-sm">
            <li v-for="k in uploader.picked.kept" :key="k.name" class="ds-upload-kept flex items-center justify-between gap-3 px-3 py-2">
              <span class="font-mono text-ink">{{ k.name }}</span>
              <span class="text-xs text-ink-muted">{{ formatBytes(k.file.size) }}</span>
            </li>
            <li v-for="name in uploader.picked.missing" :key="name" class="ds-upload-missing flex items-center justify-between gap-3 px-3 py-2">
              <span class="font-mono text-danger">{{ name }}</span>
              <span class="text-xs text-danger">missing, required</span>
            </li>
          </ul>
          <p class="text-xs text-ink-muted">
            {{ uploader.picked.kept.length }} file{{ uploader.picked.kept.length === 1 ? '' : 's' }} to upload,
            {{ formatBytes(pickedBytes) }} in all;
            <template v-if="uploader.picked.ignored > 0">{{ uploader.picked.ignored }} other file{{ uploader.picked.ignored === 1 ? '' : 's' }} left out.</template>
            <template v-else>nothing left out.</template>
          </p>
          <p v-if="uploader.picked.missing.length > 0" class="text-sm text-danger">
            The pick lacks {{ uploader.picked.missing.join(', ') }}. Pick the right folder.
          </p>
        </div>
        <div class="flex flex-wrap gap-2">
          <button
            id="ds-upload-start"
            type="button"
            :disabled="busy || !uploader.picked || uploader.picked.kept.length === 0 || uploader.picked.missing.length > 0"
            :class="primaryButton"
            @click="startUpload"
          >
            Upload
          </button>
        </div>
      </template>

      <template v-else>
        <ul class="space-y-2">
          <li v-for="f in uploader.files" :key="f.name" class="ds-upload-progress space-y-1 text-sm">
            <div class="flex items-center justify-between gap-3">
              <span class="font-mono text-ink">{{ f.name }}</span>
              <span class="text-xs text-ink-muted">{{ f.done ? 'stored' : `${formatBytes(f.uploaded)} of ${formatBytes(f.size)}` }}</span>
            </div>
            <div class="h-1.5 w-full overflow-hidden rounded-full bg-hover" role="progressbar" :aria-label="`${f.name} upload`" :aria-valuenow="percent(f.uploaded, f.size)" aria-valuemin="0" aria-valuemax="100">
              <div class="h-full bg-accent transition-all" :style="{ width: `${percent(f.uploaded, f.size)}%` }"></div>
            </div>
          </li>
        </ul>
        <p id="ds-upload-total" class="text-sm text-ink">
          <template v-if="uploader.phase === 'uploading'">
            Uploading: {{ formatBytes(uploadedBytes) }} of {{ formatBytes(pickedBytes) }} ({{ percent(uploadedBytes, pickedBytes) }}%).
          </template>
          <template v-else-if="uploader.phase === 'finalizing'">
            Every file is stored. The worker checks the files and lists the missions; this takes a little while for a large tree.
          </template>
          <template v-else-if="uploader.phase === 'ready'">
            <span class="text-online">Ready:</span> tree {{ uploader.treeId }}, version {{ uploadedTree?.version || 'unknown' }},
            {{ uploadedTree?.missions ?? 0 }} missions. New servers of {{ uploader.dataSet.display_name }} start on it.
          </template>
          <template v-else-if="uploader.phase === 'failed'">
            <span class="text-danger">The upload failed:</span> {{ uploader.error }}
          </template>
          <template v-else-if="uploader.phase === 'cancelled'">The upload was cancelled{{ uploader.error ? `: ${uploader.error}` : '.' }}</template>
        </p>
        <div class="flex flex-wrap gap-2">
          <button v-if="uploader.phase === 'uploading'" id="ds-upload-cancel" type="button" :class="secondaryButton" @click="cancelUpload">Cancel the upload</button>
          <button v-else-if="uploader.phase !== 'finalizing'" type="button" :class="secondaryButton" @click="closeUploader">Close</button>
        </div>
      </template>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/data-sets: the hosted data sets (GET /api/admin/data-sets) with
// their trees, the new-data-set form (POST), each one's edit (PATCH), delete,
// and its servers' update (POST /api/admin/hosted/update-all); and the
// uploader (a folder picked in the browser, its files kept to the kind's list
// and sent through the site to the store in parts: POST trees, PUT each part,
// POST complete per file, POST finalize, then the page polls the tree to
// ready or failed). The page polls, faster while a tree is being finalized.
import { computed, onBeforeUnmount, nextTick, reactive, ref } from 'vue';
import RelativeTime from '../../components/RelativeTime.vue';
import {
  completeTreeFile,
  createDataSet,
  createTree,
  deleteDataSet,
  deleteTree,
  fetchDataSets,
  finalizeTree,
  putTreePart,
  updateAllServers,
  updateDataSet,
} from '../../api/dataSets';
import { usePolling } from '../../composables/usePolling';
import type { DataSet, DataSetKind, DataSetsResponse, NewDataSet, Tree } from '../../types/dataSets';
import { formatBytes, formatMemory } from '../../utils/format';
import { isAbortError, uploadFile } from '../../utils/partUpload';
import { pickTreeFiles, treeFileRules, type PickedTreeFiles } from '../../utils/treeFiles';

// The part size the API hands out (32 MiB; what web's nginx takes in one
// request body), for the header's note.
const PART_SIZE = 32 * 1024 * 1024;
const BUSY_POLL_MS = 3_000;
const IDLE_POLL_MS = 30_000;
const TITLE_NAMES: Record<string, string> = { jo: 'Joint Operations', dfx2: 'Delta Force Xtreme 2' };
const EXPANSION_CODE = /^[a-z0-9]{1,8}$/;

const labelClass = 'block text-xs font-medium uppercase tracking-wider text-ink-muted';
const inputClass = 'w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none aria-[invalid=true]:border-danger';
const primaryButton = 'rounded-control bg-accent px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-40';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const data = ref<DataSetsResponse | null>(null);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
// The error banner shows a failed read (not an action's refusal).
let errorFromLoad = false;

const expanded = reactive(new Set<number>());
const bases = computed(() => (data.value?.data_sets ?? []).filter((d) => d.kind === 'base'));

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
  errorFromLoad = false;
}

// A tree is being finalized: the page reads again soon.
const working = computed(() => {
  const trees = (data.value?.data_sets ?? []).some((d) => d.trees.some((t) => t.state === 'finalizing'));
  return trees || uploader.value?.phase === 'finalizing';
});

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchDataSets(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      data.value = result.value;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (errorFromLoad) {
        error.value = '';
        errorFromLoad = false;
      }
      followUpload();
      return true;
    }
    if (background && data.value !== null) {
      pollTrouble.value = true;
    } else {
      fail(result.error.message);
      errorFromLoad = true;
    }
    return false;
  },
  next: () => (working.value ? BUSY_POLL_MS : IDLE_POLL_MS),
});

function titleName(code: string): string {
  return TITLE_NAMES[code] ?? code.toUpperCase();
}

function gameName(slug: string): string {
  return data.value?.games.find((g) => g.slug === slug)?.display_name ?? slug;
}

// "Base game: Joint Operations, lobby Joint Operations" or "Expansion jox01
// of Joint Operations".
function kindLine(d: DataSet): string {
  if (d.kind === 'base') return `Base game ${titleName(d.title)}, lobby ${gameName(d.game)}`;
  const parent = data.value?.data_sets.find((p) => p.id === d.parent_id);
  return `Expansion ${d.expansion_code} of ${parent?.display_name ?? `data set ${d.parent_id}`}`;
}

function creatorLabel(createdBy: string): string {
  if (createdBy === 'token') return 'by the admin token';
  return createdBy.startsWith('admin ') ? `by ${createdBy.slice(6)}` : createdBy;
}

function treeStateLabel(state: string): string {
  return ({ uploading: 'Uploading', finalizing: 'Finalizing', ready: 'Ready', failed: 'Failed', pruned: 'Pruned' } as Record<string, string>)[state] ?? state;
}

function treeStateClass(state: string): string {
  const base = 'rounded-control px-2 py-0.5 text-xs font-medium';
  if (state === 'ready') return `${base} bg-online/10 text-online`;
  if (state === 'failed') return `${base} bg-danger/10 text-danger`;
  if (state === 'pruned') return `${base} bg-raised text-ink-muted`;
  return `${base} bg-warn/10 text-warn`;
}

function percent(done: number, total: number): number {
  if (total <= 0) return done > 0 ? 100 : 0;
  return Math.min(100, Math.floor((done / total) * 100));
}

function toggleTrees(d: DataSet) {
  if (expanded.has(d.id)) expanded.delete(d.id);
  else expanded.add(d.id);
}

// ---------------------------------------------------------------------------
// The new data set.

interface NewForm {
  kind: DataSetKind;
  display_name: string;
  title: string;
  game: string;
  parent_id: number;
  expansion_code: string;
}

const creating = ref(false);
const newForm = reactive<NewForm>({
  kind: 'base',
  display_name: '',
  title: '',
  game: '',
  parent_id: 0,
  expansion_code: '',
});
const newErrors = ref<Partial<Record<keyof NewForm, string>>>({});

function toggleNewForm() {
  if (creating.value) {
    creating.value = false;
    return;
  }
  const d = data.value;
  if (!d) return;
  Object.assign(newForm, {
    kind: 'base',
    display_name: '',
    title: d.titles[0] ?? '',
    game: d.games[0]?.slug ?? '',
    parent_id: bases.value[0]?.id ?? 0,
    expansion_code: '',
  });
  newErrors.value = {};
  creating.value = true;
}

// The form against the routes' rules; the body to send when it passes.
function checkNewForm(): NewDataSet | null {
  const errors: Partial<Record<keyof NewForm, string>> = {};
  const displayName = newForm.display_name.trim();
  if (displayName === '') errors.display_name = 'Enter a display name.';
  const body: NewDataSet = { display_name: displayName, kind: newForm.kind };
  if (newForm.kind === 'base') {
    if (!data.value?.titles.includes(newForm.title)) errors.title = 'Pick the title the game speaks.';
    if (!data.value?.games.some((g) => g.slug === newForm.game)) errors.game = 'Pick the lobby it lists in.';
    body.title = newForm.title;
    body.game = newForm.game;
  } else {
    const code = newForm.expansion_code.trim().toLowerCase();
    if (!bases.value.some((b) => b.id === newForm.parent_id)) errors.parent_id = 'Pick the base game it belongs to.';
    if (!EXPANSION_CODE.test(code)) errors.expansion_code = 'A code is 1 to 8 lowercase letters and digits (jox01).';
    body.parent_id = newForm.parent_id;
    body.expansion_code = code;
  }
  newErrors.value = errors;
  return Object.keys(errors).length === 0 ? body : null;
}

async function create() {
  const body = checkNewForm();
  if (!body || busy.value) return;
  busy.value = true;
  const result = await createDataSet(body);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  creating.value = false;
  flash(`${result.value.display_name} is created. Upload its data next.`);
  await poll.readAgain();
}

// ---------------------------------------------------------------------------
// One data set's edit, servers' update and delete.

interface EditForm {
  id: number;
  kind: DataSetKind;
  display_name: string;
  game: string;
}

const editing = ref<EditForm | null>(null);
const editError = ref('');

function toggleEdit(d: DataSet) {
  if (editing.value?.id === d.id) {
    editing.value = null;
    return;
  }
  editError.value = '';
  editing.value = { id: d.id, kind: d.kind, display_name: d.display_name, game: d.game };
}

async function saveEdit() {
  const e = editing.value;
  if (!e || busy.value) return;
  const displayName = e.display_name.trim();
  editError.value = displayName === '' ? 'Enter a display name.' : '';
  if (editError.value) return;
  busy.value = true;
  const result = await updateDataSet(e.id, { display_name: displayName, ...(e.kind === 'base' ? { game: e.game } : {}) });
  busy.value = false;
  if (!result.ok) {
    editError.value = result.error.message;
    return;
  }
  editing.value = null;
  flash(`${result.value.display_name} is saved.`);
  await poll.readAgain();
}

async function updateServers(d: DataSet) {
  if (busy.value) return;
  const n = d.servers_outdated;
  if (!window.confirm(`Update the ${n} outdated server${n === 1 ? '' : 's'} of ${d.display_name}? Each restarts on the current data, and its players drop while it does.`)) {
    return;
  }
  busy.value = true;
  const result = await updateAllServers(d.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  const asked = result.value;
  flash(asked === 0 ? `No server of ${d.display_name} runs older data.` : `${asked} server${asked === 1 ? ' is' : 's are'} updating.`);
  await poll.readAgain();
}

async function removeDataSet(d: DataSet) {
  if (busy.value) return;
  if (!window.confirm(`Delete ${d.display_name}? Its trees and their files are removed. A data set with a live server or an expansion stays.`)) return;
  busy.value = true;
  const result = await deleteDataSet(d.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  expanded.delete(d.id);
  if (uploader.value?.dataSet.id === d.id && !uploading.value) uploader.value = null;
  flash(`${d.display_name} is deleted.`);
  await poll.readAgain();
}

async function removeTree(d: DataSet, t: Tree) {
  if (busy.value) return;
  const what = t.state === 'uploading' ? 'Its upload is abandoned.' : 'Its files are removed from the store.';
  if (!window.confirm(`Delete tree ${t.id} of ${d.display_name}${t.version ? ` (${t.version})` : ''}? ${what}`)) return;
  busy.value = true;
  const result = await deleteTree(d.id, t.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  flash(`Tree ${t.id} is ${t.state === 'uploading' ? 'deleted' : 'being pruned'}.`);
  await poll.readAgain();
}

// ---------------------------------------------------------------------------
// The uploader.

interface UploadRow {
  name: string;
  size: number;
  uploaded: number;
  done: boolean;
}

interface Uploader {
  dataSet: DataSet;
  phase: 'pick' | 'uploading' | 'finalizing' | 'ready' | 'failed' | 'cancelled';
  picked: PickedTreeFiles | null;
  files: UploadRow[];
  treeId: number | null;
  error: string;
  controller: AbortController | null;
}

const uploader = ref<Uploader | null>(null);
const uploading = computed(() => uploader.value?.phase === 'uploading' || uploader.value?.phase === 'finalizing');
const pickedBytes = computed(() => (uploader.value?.picked?.kept ?? []).reduce((sum, k) => sum + k.file.size, 0));
const uploadedBytes = computed(() => (uploader.value?.files ?? []).reduce((sum, f) => sum + f.uploaded, 0));
// The tree being uploaded, as the last read has it.
const uploadedTree = computed<Tree | null>(() => {
  const u = uploader.value;
  if (!u || u.treeId === null) return null;
  return data.value?.data_sets.find((d) => d.id === u.dataSet.id)?.trees.find((t) => t.id === u.treeId) ?? null;
});

// "resource.pff, localres.pff and language.pff (required), version.txt, ...".
function ruleList(d: DataSet): string {
  const rules = treeFileRules(d.kind, d.expansion_code);
  const required = rules.filter((r) => r.required).map((r) => r.name);
  const optional = rules.filter((r) => !r.required).map((r) => r.name);
  const join = (names: string[]) => (names.length <= 1 ? names.join('') : `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`);
  return optional.length > 0 ? `${join(required)} (required), with ${join(optional)} when present` : join(required);
}

async function openUploader(d: DataSet) {
  if (uploading.value) return;
  uploader.value = { dataSet: d, phase: 'pick', picked: null, files: [], treeId: null, error: '', controller: null };
  await nextTick();
  document.getElementById('ds-uploader-heading')?.focus();
}

function closeUploader() {
  if (uploading.value) return;
  uploader.value = null;
}

function onPicked(event: Event) {
  const u = uploader.value;
  const input = event.target as HTMLInputElement;
  if (!u || u.phase !== 'pick') return;
  const files = Array.from(input.files ?? []);
  u.picked = files.length > 0 ? pickTreeFiles(u.dataSet.kind, u.dataSet.expansion_code, files) : null;
}

// The whole upload: the tree opened, each file sent in parts through the
// site and completed, then finalize. A failure or a cancel deletes the
// half-uploaded tree.
async function startUpload() {
  const u = uploader.value;
  if (!u || !u.picked || u.picked.missing.length > 0 || u.picked.kept.length === 0 || busy.value) return;
  const { dataSet, picked } = u;
  const controller = new AbortController();
  u.controller = controller;
  u.error = '';
  u.files = picked.kept.map((k) => ({ name: k.name, size: k.file.size, uploaded: 0, done: false }));
  u.phase = 'uploading';
  const opened = await createTree(
    dataSet.id,
    picked.kept.map((k) => ({ name: k.name, size: k.file.size })),
  );
  if (!opened.ok) {
    u.phase = 'failed';
    u.error = opened.error.message;
    return;
  }
  const treeId = opened.value.tree.id;
  u.treeId = treeId;
  try {
    for (const kept of picked.kept) {
      const spec = opened.value.uploads.find((x) => x.name.toLowerCase() === kept.name.toLowerCase());
      if (!spec) throw new Error(`The service opened no upload for ${kept.name}.`);
      const row = u.files.find((f) => f.name === kept.name)!;
      const parts = await uploadFile(kept.file, {
        partSize: spec.part_size,
        signal: controller.signal,
        putPart: async (partNumber, blob, signal) => {
          const stored = await putTreePart(dataSet.id, treeId, spec.name, spec.upload_id, partNumber, blob, signal);
          if (!stored.ok) throw new Error(stored.error.message);
          return { etag: stored.value.etag };
        },
        onProgress: (bytes) => {
          row.uploaded = bytes;
        },
      });
      const completed = await completeTreeFile(dataSet.id, treeId, spec.name, { upload_id: spec.upload_id, parts });
      if (!completed.ok) throw new Error(completed.error.message);
      row.uploaded = row.size;
      row.done = true;
    }
    u.phase = 'finalizing';
    const finalized = await finalizeTree(dataSet.id, treeId);
    if (!finalized.ok) throw new Error(finalized.error.message);
    flash(`Every file of ${dataSet.display_name} is stored; the worker finalizes tree ${treeId}.`);
    await poll.readAgain();
  } catch (err) {
    const cancelled = isAbortError(err);
    u.phase = cancelled ? 'cancelled' : 'failed';
    u.error = cancelled ? '' : err instanceof Error ? err.message : 'Something went wrong.';
    const removed = await deleteTree(dataSet.id, treeId);
    if (removed.ok) {
      u.error = cancelled ? 'the half-uploaded tree was removed.' : `${u.error} The half-uploaded tree was removed.`;
    } else {
      u.error = `${u.error}${u.error ? ' ' : ''}Tree ${treeId} stays half uploaded: delete it from the trees list (${removed.error.message})`;
    }
    await poll.readAgain();
  } finally {
    u.controller = null;
  }
}

function cancelUpload() {
  uploader.value?.controller?.abort();
}

// The tree's outcome once the worker finalized it: the page says so and
// stops polling fast.
function followUpload() {
  const u = uploader.value;
  if (!u || u.phase !== 'finalizing') return;
  const tree = uploadedTree.value;
  if (!tree) return;
  if (tree.state === 'ready') {
    u.phase = 'ready';
    flash(`${u.dataSet.display_name} ${tree.version || `tree ${tree.id}`} is ready: new servers start on it.`);
  } else if (tree.state === 'failed') {
    u.phase = 'failed';
    u.error = tree.error || 'the worker refused the tree.';
  }
}

// Leaving the page mid-upload abandons it: the browser asks first, and the
// parts in flight are aborted on the way out.
function onBeforeUnload(event: BeforeUnloadEvent) {
  if (uploading.value) event.preventDefault();
}
window.addEventListener('beforeunload', onBeforeUnload);
onBeforeUnmount(() => {
  window.removeEventListener('beforeunload', onBeforeUnload);
  uploader.value?.controller?.abort();
});
</script>
