<template>
  <div class="space-y-8">
    <PageHeader
      :title="product?.display_name ?? (missing ? 'No such product' : 'Product')"
      :crumbs="[{ label: 'Products', to: '/admin/products' }]"
      refreshable
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="poll.refresh"
    >
      <template v-if="product" #meta>
        <span class="product-kind">{{ kindLine(product, products, data?.games ?? []) }}</span>
        <span v-if="product.protocol" class="product-protocol">{{ protocolWords(product.protocol) }}</span>
        <span v-if="product.key" class="font-mono">{{ product.key }}</span>
        <span class="product-owner">
          <PlayerLink v-if="product.owner" :id="product.owner_id" :name="product.owner" />
          <template v-else>OpenNova</template>
        </span>
        <Badge v-if="product.visibility" class="product-visibility" :tone="product.visibility === 'public' ? 'info' : 'muted'">
          {{ product.visibility === 'public' ? 'Public' : 'Private' }}
        </Badge>
      </template>
      <template v-if="product && product.kind !== 'app'" #actions>
        <button id="product-edit-toggle" type="button" class="btn-secondary" :disabled="busy" :aria-expanded="editing ? 'true' : 'false'" @click="toggleEdit">
          Edit…
        </button>
        <button id="product-delete-toggle" type="button" class="btn-danger" :disabled="busy" :aria-expanded="deleting ? 'true' : 'false'" @click="toggleDelete">
          Delete…
        </button>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" :poll-trouble="pollTrouble" />

    <Notice v-if="missing" class="product-missing">
      No product has the key or id {{ productRef }}. <RouterLink to="/admin/products" class="link">All products</RouterLink>
    </Notice>
    <p v-else-if="!product" class="text-sm text-ink-muted">Loading…</p>

    <template v-else>
      <form v-if="editing" id="product-edit-form" class="card space-y-4 p-4 sm:p-6" novalidate @submit.prevent="saveEdit">
        <div class="grid gap-4 sm:grid-cols-2 lg:grid-cols-4">
          <div>
            <label for="product-edit-display_name" class="field-label">Name</label>
            <input id="product-edit-display_name" v-model="editing.display_name" type="text" autocomplete="off" maxlength="64" class="field" />
          </div>
          <div v-if="product.kind === 'game'">
            <label for="product-edit-lobby" class="field-label">Lobby</label>
            <select id="product-edit-lobby" v-model="editing.lobby" class="field" :disabled="busy || product.lobby_locked">
              <option v-for="g in editLobbies" :key="g.slug" :value="g.slug">{{ g.display_name }}</option>
            </select>
            <p v-if="product.lobby_locked" class="mt-1 text-xs text-ink-muted">Locked after the first release build.</p>
          </div>
          <div>
            <label for="product-edit-updater_login" class="field-label">Updater login</label>
            <input id="product-edit-updater_login" v-model="editing.updater_login" type="text" autocomplete="off" spellcheck="false" maxlength="32" class="field font-mono" />
          </div>
          <div>
            <label for="product-edit-command_file" class="field-label">Command file</label>
            <input id="product-edit-command_file" v-model="editing.command_file" type="text" autocomplete="off" spellcheck="false" maxlength="15" class="field font-mono" />
          </div>
        </div>
        <p v-if="editError" id="product-edit-error" class="text-xs text-danger">{{ editError }}</p>
        <div class="flex flex-wrap gap-2">
          <button type="submit" class="btn-primary" :disabled="busy">Save</button>
          <button type="button" class="btn-secondary" :disabled="busy" @click="editing = null">Cancel</button>
        </div>
      </form>

      <form v-if="deleting" id="product-delete-form" class="card space-y-3 border-danger/40 p-4 sm:p-6" @submit.prevent="confirmDelete">
        <p class="text-sm font-semibold text-danger">Delete {{ product.display_name }}? Its releases, servers' data and project history go.</p>
        <div>
          <label for="product-delete-confirm" class="field-label">Type <span class="font-mono font-semibold text-ink">{{ confirmWord }}</span> to confirm</label>
          <input id="product-delete-confirm" v-model="typed" type="text" autocomplete="off" spellcheck="false" class="field max-w-xs font-mono" />
        </div>
        <div class="flex flex-wrap gap-2">
          <button type="submit" class="btn-danger" :disabled="busy || typed.trim() !== confirmWord">{{ busy ? 'Deleting…' : 'Delete now' }}</button>
          <button type="button" class="btn-secondary" :disabled="busy" @click="deleting = false">Cancel</button>
        </div>
      </form>

      <template v-if="product.kind !== 'app'">
        <dl class="product-tiles grid grid-cols-1 gap-3 min-[420px]:grid-cols-3">
          <StatTile label="Server release" :value="product.current_tree?.version || 'None ready'" />
          <StatTile label="Live servers" :value="formatCount(product.servers_live)" :to="serversLink" />
          <StatTile label="Outdated" :value="formatCount(product.servers_outdated)" :tone="product.servers_outdated > 0 ? 'warn' : undefined" />
        </dl>
        <button v-if="product.servers_outdated > 0 && product.current_tree" id="product-update-servers" type="button" class="btn-warn btn-sm" :disabled="busy" @click="updateServers">
          Move {{ product.servers_outdated }} to {{ product.current_tree.version || 'the published release' }}
        </button>
      </template>

      <section v-if="product.kind !== 'app'" class="space-y-3" aria-labelledby="product-project">
        <h2 id="product-project" class="section-heading">Project</h2>
        <template v-if="product.key">
          <div class="flex flex-wrap items-center gap-x-4 gap-y-2">
            <p id="product-project-line" class="min-w-0 text-sm text-ink">
              <template v-if="product.head > 0">
                <span class="font-mono">r{{ product.head }}</span><template v-if="newest"> by {{ newest.author }}: <q>{{ newest.message }}</q></template>
              </template>
              <span v-else class="text-ink-muted">Nothing pushed yet.</span>
            </p>
            <div class="flex flex-wrap gap-2">
              <button
                v-if="product.head > 0"
                id="product-history-toggle"
                type="button"
                class="btn-secondary btn-sm"
                :aria-expanded="showHistory ? 'true' : 'false'"
                @click="showHistory = !showHistory"
              >
                History
              </button>
              <button id="product-visibility" type="button" class="btn-secondary btn-sm" :disabled="busy" @click="toggleVisibility">
                {{ product.visibility === 'public' ? 'Make private' : 'Make public' }}
              </button>
              <button id="product-rename-toggle" type="button" class="btn-secondary btn-sm" :disabled="busy" :aria-expanded="renaming ? 'true' : 'false'" @click="toggleRename">
                Rename…
              </button>
            </div>
          </div>
          <form v-if="renaming" id="product-rename-form" class="flex flex-wrap items-end gap-2" novalidate @submit.prevent="saveRename">
            <div>
              <label for="product-rename-key" class="field-label">New key</label>
              <input
                id="product-rename-key"
                v-model="newKey"
                type="text"
                autocomplete="off"
                autocapitalize="characters"
                spellcheck="false"
                maxlength="16"
                title="Project files naming the old key stop finding it"
                :aria-invalid="renameProblem ? 'true' : undefined"
                class="field w-48 font-mono"
              />
            </div>
            <button type="submit" class="btn-primary btn-sm" :disabled="busy || newKey.trim().toUpperCase() === product.key">Save key</button>
            <button type="button" class="btn-ghost btn-sm" @click="renaming = false">Cancel</button>
            <p v-if="renameProblem" class="w-full text-xs text-danger">{{ renameProblem }}</p>
          </form>
          <div v-if="showHistory" id="product-history" class="space-y-2">
            <p v-if="history.revisions.length === 0" class="text-sm text-ink-muted">{{ history.loading ? 'Loading…' : 'Nothing pushed yet.' }}</p>
            <ol v-else class="table-wrap divide-y divide-border">
              <li v-for="r in history.revisions" :key="r.number" class="product-revision flex flex-wrap items-baseline gap-x-3 gap-y-1 px-4 py-2 text-sm">
                <span class="font-mono text-xs font-semibold">r{{ r.number }}</span>
                <span class="min-w-0 flex-1 break-words text-ink">{{ r.message }}</span>
                <span class="text-xs text-ink-muted">
                  {{ r.author }}, {{ r.file_count }} file{{ r.file_count === 1 ? '' : 's' }}, {{ formatBytes(r.bytes) }},
                  <RelativeTime :value="r.created_at" :now="now" />
                </span>
              </li>
            </ol>
            <button v-if="history.more" type="button" class="btn-secondary btn-sm" :disabled="history.loading" @click="olderRevisions">
              {{ history.loading ? 'Loading…' : 'Older revisions' }}
            </button>
          </div>
        </template>
        <p v-else class="text-sm text-ink-muted">No project.</p>
      </section>

      <section class="space-y-3" aria-labelledby="product-releases">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <h2 id="product-releases" class="section-heading">Releases</h2>
          <button
            v-if="product.kind !== 'app' && product.head > 0 && !building"
            id="product-build-toggle"
            type="button"
            class="btn-secondary btn-sm"
            :disabled="busy"
            @click="beginBuild"
          >
            Release r{{ product.head }}…
          </button>
        </div>

        <form v-if="building" id="product-build-form" class="flex flex-wrap items-end gap-2" novalidate @submit.prevent="confirmBuild">
          <div>
            <label for="product-build-version" class="field-label">Game version</label>
            <input
              id="product-build-version"
              v-model="buildVersion"
              type="text"
              autocomplete="off"
              spellcheck="false"
              placeholder="0.2.0"
              title="One to four numbers between dots (0.2.0)"
              maxlength="32"
              :aria-invalid="versionProblem ? 'true' : undefined"
              class="field w-32 font-mono"
            />
          </div>
          <div>
            <label for="product-build-revision" class="field-label">Revision</label>
            <input id="product-build-revision" v-model.number="buildRevision" type="number" min="1" :max="product.head" class="field w-24 font-mono" />
          </div>
          <button type="submit" class="btn-primary btn-sm" :disabled="busy || !buildVersion.trim()">{{ busy ? 'Starting…' : 'Build release' }}</button>
          <button type="button" class="btn-ghost btn-sm" :disabled="busy" @click="building = false">Cancel</button>
          <p v-if="versionProblem" class="w-full text-xs text-danger">{{ versionProblem }}</p>
        </form>

        <div class="table-wrap">
          <table class="data-table">
            <caption class="sr-only">Releases of {{ product.display_name }}, newest first</caption>
            <thead>
              <tr>
                <th scope="col">Release</th>
                <th scope="col">Status</th>
                <th scope="col">Package</th>
                <th scope="col">Made</th>
                <th scope="col"><span class="sr-only">Actions</span></th>
              </tr>
            </thead>
            <tbody>
              <TableState v-if="ownReleases.length === 0" :colspan="5" :rows="ownReleases" empty="No releases yet." />
              <template v-for="r in ownReleases" :key="r.id">
                <tr class="product-release">
                  <th scope="row">
                    <span class="product-release-version font-mono">{{ releaseLabel(r) }}</span>
                    <span v-if="r.one_version" class="product-release-one block text-xs font-normal text-ink-muted">ONE {{ r.one_version }}</span>
                    <span v-if="r.source" class="font-normal text-ink-muted">
                      (r{{ r.source.revision }}<template v-if="r.source.base_revision">, base r{{ r.source.base_revision }}</template>)
                    </span>
                  </th>
                  <td>
                    <span :class="releaseStatusClass(r.status)">{{ releaseStatusLabel(r.status) }}</span>
                    <span v-if="r.error" class="block max-w-xs whitespace-normal text-xs text-danger">{{ r.error }}</span>
                    <span v-if="r.status === 'draft' && r.review_note" class="product-release-note block max-w-xs whitespace-normal text-xs text-warn">Sent back: {{ r.review_note }}</span>
                    <span v-if="treeOf(r)" class="block text-xs text-ink-muted">{{ treeStateText(treeOf(r)!) }}</span>
                    <details v-if="r.build_log" class="product-release-log mt-1 max-w-md text-xs" :open="r.status === 'building'">
                      <summary class="text-accent hover:text-ink-bright">Build log</summary>
                      <pre class="mt-1 max-h-64 overflow-auto whitespace-pre-wrap break-all rounded-control border border-border bg-surface p-2 font-mono text-ink">{{ r.build_log }}</pre>
                    </details>
                  </td>
                  <td class="text-xs">
                    <span class="block">{{ r.bytes > 0 ? formatBytes(r.bytes) : 'Not packed yet' }}</span>
                    <span v-if="r.sha256" class="block font-mono text-ink-muted">{{ r.sha256.slice(0, 12) }}</span>
                  </td>
                  <td class="whitespace-nowrap text-xs text-ink-muted">
                    <RelativeTime :value="r.created_at" :now="now" />
                    <span class="product-release-creator block">
                      {{ releaseCreator(r.created_by).words }}
                      <PlayerLink v-if="releaseCreator(r.created_by).account" :id="creatorId(r)" :name="releaseCreator(r.created_by).account!" />
                    </span>
                  </td>
                  <td class="whitespace-nowrap text-right">
                    <button v-if="r.status === 'draft'" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="publish(r)">
                      Publish<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                    </button>
                    <button v-if="r.status === 'awaiting_approval'" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="publish(r)">
                      Approve<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                    </button>
                    <button v-if="r.status === 'awaiting_approval'" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="beginReject(r)">
                      Reject…<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                    </button>
                    <button v-if="r.status === 'published'" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="retire(r)">
                      Retire<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                    </button>
                    <button
                      v-if="r.status === 'draft' || r.status === 'invalid' || r.status === 'retired'"
                      type="button"
                      class="btn-ghost btn-sm"
                      :disabled="busy"
                      @click="removeRelease(r)"
                    >
                      Delete<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                    </button>
                  </td>
                </tr>
                <tr v-if="rejecting?.id === r.id">
                  <td colspan="5">
                    <form id="product-reject-form" class="flex flex-wrap items-end gap-2" @submit.prevent="confirmReject">
                      <div class="min-w-0 flex-1">
                        <label for="product-reject-note" class="field-label">Send {{ releaseLabel(r) }} back to {{ product.owner ?? 'its owner' }}: note (optional)</label>
                        <input id="product-reject-note" v-model="rejectNote" type="text" autocomplete="off" class="field" :disabled="busy" />
                      </div>
                      <button type="submit" class="btn-warn btn-sm" :disabled="busy">{{ busy ? 'Sending…' : 'Send back' }}</button>
                      <button type="button" class="btn-ghost btn-sm" :disabled="busy" @click="rejecting = null">Cancel</button>
                    </form>
                  </td>
                </tr>
              </template>
            </tbody>
          </table>
        </div>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/products/<key or id>: one product: its settings and deletion, its
// project (history, visibility, key), its releases (build, publish, approve or
// send back, retire, delete) and its servers moved to the published release.
import { computed, onMounted, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import RelativeTime from '../../../components/RelativeTime.vue';
import StatTile from '../../../components/charts/StatTile.vue';
import PlayerLink from '../../../components/links/PlayerLink.vue';
import Badge from '../../../components/ui/Badge.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import Notice from '../../../components/ui/Notice.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TableState from '../../../components/ui/TableState.vue';
import {
  REVISIONS_PAGE,
  buildRelease,
  deleteProduct,
  deleteRelease,
  fetchProducts,
  fetchReleases,
  fetchRevisions,
  publishRelease,
  rejectRelease,
  retireRelease,
  updateAllServers,
  updateProduct,
} from '../../../api/products';
import { listUsers } from '../../../api/users';
import { confirmAction } from '../../../composables/useConfirm';
import { usePolling } from '../../../composables/usePolling';
import { useUnsavedChanges } from '../../../composables/useUnsavedChanges';
import type { ProductsResponse, ProjectRevision, Release } from '../../../types/products';
import { formatBytes, formatCount } from '../../../utils/format';
import { protocolWords } from '../../../utils/games';
import {
  KEY_RULE,
  VERSION_RULE,
  findProduct,
  kindLine,
  productLink,
  releaseCreator,
  releaseLabel,
  releaseStatusClass,
  releaseStatusLabel,
  releasesOf,
  releasesWorking,
  treeStateText,
  updaterProblems,
} from '../../../utils/productsView';

const BUSY_POLL_MS = 3_000;
const IDLE_POLL_MS = 30_000;

const route = useRoute();
const router = useRouter();
const productRef = computed(() => String(route.params.ref ?? ''));

const data = ref<ProductsResponse | null>(null);
const releases = ref<Release[]>([]);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
let errorFromLoad = false;
// Account ids by lower-case username, for the releases' creators (read once).
const accounts = ref(new Map<string, number>());

const products = computed(() => data.value?.products ?? []);
const product = computed(() => findProduct(products.value, productRef.value));
const missing = computed(() => data.value !== null && product.value === null);
const ownReleases = computed(() => (product.value ? releasesOf(product.value, releases.value) : []));
const serversLink = computed(() => ({ path: '/admin/servers', query: { product: product.value?.id } }));

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
  errorFromLoad = false;
}

const poll = usePolling({
  async read(signal, background) {
    const [sets, list] = await Promise.all([fetchProducts(signal), fetchReleases(signal)]);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (sets.ok && list.ok) {
      data.value = sets.value;
      releases.value = list.value;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (errorFromLoad) {
        error.value = '';
        errorFromLoad = false;
      }
      return true;
    }
    const failure = !sets.ok ? sets.error : !list.ok ? list.error : null;
    if (background && data.value !== null) {
      pollTrouble.value = true;
    } else if (failure) {
      fail(failure.message);
      errorFromLoad = true;
    }
    return false;
  },
  next: () => (releasesWorking(ownReleases.value) ? BUSY_POLL_MS : IDLE_POLL_MS),
});

onMounted(async () => {
  const result = await listUsers();
  if (result.ok) accounts.value = new Map(result.value.map((u) => [u.username.toLowerCase(), u.id]));
});

// The account that started a release: its owner's id, else the users list's.
function creatorId(r: Release): number | null {
  const name = releaseCreator(r.created_by).account;
  if (!name) return null;
  const g = product.value;
  if (g?.owner && g.owner_id !== null && g.owner.toLowerCase() === name.toLowerCase()) return g.owner_id;
  return accounts.value.get(name.toLowerCase()) ?? null;
}

// The tree a published release's section of this product built, by its state.
function treeOf(r: Release): string | null {
  if (r.status !== 'published') return null;
  return r.sections.find((s) => s.product_id === product.value?.id)?.tree?.state ?? null;
}

// ---------------------------------------------------------------------------
// The project's history: its first page read for the newest revision, the
// rest when asked.

const history = ref<{ key: string; head: number; revisions: ProjectRevision[]; more: boolean; loading: boolean }>({
  key: '',
  head: 0,
  revisions: [],
  more: false,
  loading: false,
});
const showHistory = ref(false);
const newest = computed(() => (history.value.key === product.value?.key ? (history.value.revisions[0] ?? null) : null));

async function readHistory(before?: number) {
  const shown = history.value;
  shown.loading = true;
  const result = await fetchRevisions(shown.key, before);
  if (history.value !== shown) return;
  shown.loading = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  shown.revisions = before ? [...shown.revisions, ...result.value] : result.value;
  const last = shown.revisions[shown.revisions.length - 1];
  shown.more = result.value.length === REVISIONS_PAGE && last !== undefined && last.number > 1;
}

watch(
  () => [product.value?.kind === 'app' ? null : (product.value?.key ?? null), product.value?.head ?? 0] as const,
  ([key, head]) => {
    if (key === null || head === 0) return;
    if (history.value.key === key && history.value.head === head) return;
    history.value = { key, head, revisions: [], more: false, loading: true };
    void readHistory();
  },
  { immediate: true },
);

function olderRevisions() {
  const last = history.value.revisions[history.value.revisions.length - 1];
  if (last) void readHistory(last.number);
}

// Another product's page in the same view starts with every form closed.
watch(productRef, () => {
  editing.value = null;
  deleting.value = false;
  renaming.value = false;
  building.value = false;
  rejecting.value = null;
  showHistory.value = false;
});

// ---------------------------------------------------------------------------
// The product's settings and its deletion.

interface EditForm {
  display_name: string;
  lobby: string;
  updater_login: string;
  command_file: string;
}

const editing = ref<EditForm | null>(null);
const editError = ref('');
const editLobbies = computed(() => {
  const g = product.value;
  // The lobbies of the protocol the game's title speaks, and the one it lists in.
  const options = (data.value?.games ?? []).filter((lobby) => !!g?.protocol && lobby.protocol.code === g.protocol.code);
  return g?.lobby && g.protocol && !options.some((lobby) => lobby.slug === g.lobby)
    ? [{ slug: g.lobby, display_name: g.lobby, protocol: g.protocol }, ...options] : options;
});

function toggleEdit() {
  const g = product.value;
  if (editing.value || !g) {
    editing.value = null;
    return;
  }
  deleting.value = false;
  editError.value = '';
  editing.value = {
    display_name: g.display_name,
    lobby: g.lobby,
    updater_login: g.updater_login,
    command_file: g.command_file,
  };
}

async function saveEdit() {
  const e = editing.value;
  const g = product.value;
  if (!e || !g || busy.value) return;
  const displayName = e.display_name.trim();
  const login = e.updater_login.trim();
  const commandFile = e.command_file.trim().toUpperCase();
  const problems = updaterProblems(login, commandFile);
  editError.value = displayName === '' ? 'Enter a name.' : (problems.updater_login ?? problems.command_file ?? '');
  if (editError.value) return;
  busy.value = true;
  const result = await updateProduct(g.id, {
    display_name: displayName,
    ...(g.kind === 'game' && !g.lobby_locked ? { lobby: e.lobby } : {}),
    updater_login: login,
    command_file: commandFile,
  });
  busy.value = false;
  if (!result.ok) {
    editError.value = result.error.message;
    if (result.error.code === 'lobby_locked') await poll.readAgain();
    return;
  }
  editing.value = null;
  flash(`${result.value.display_name} is saved.`);
  await poll.readAgain();
}

const deleting = ref(false);
const typed = ref('');
const confirmWord = computed(() => product.value?.key ?? '');

function toggleDelete() {
  editing.value = null;
  deleting.value = !deleting.value;
  typed.value = '';
}

async function confirmDelete() {
  const g = product.value;
  if (!g || busy.value || typed.value.trim() !== confirmWord.value) return;
  busy.value = true;
  const result = await deleteProduct(g.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  editing.value = null;
  renaming.value = false;
  building.value = false;
  await router.push('/admin/products');
}

// ---------------------------------------------------------------------------
// The project's visibility and key.

async function toggleVisibility() {
  const g = product.value;
  if (!g || g.kind === 'app' || busy.value) return;
  const visibility = g.visibility === 'public' ? 'private' : 'public';
  busy.value = true;
  const result = await updateProduct(g.id, { visibility });
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  flash(`${g.key} is ${visibility}.`);
  await poll.readAgain();
}

const renaming = ref(false);
const newKey = ref('');
const renameTried = ref(false);
const renameProblem = computed(() => (renameTried.value && !KEY_RULE.test(newKey.value.trim().toUpperCase()) ? 'A key is 1 to 16 of A-Z, 0-9 and _.' : ''));

function toggleRename() {
  renaming.value = !renaming.value;
  newKey.value = product.value?.key ?? '';
  renameTried.value = false;
}

async function saveRename() {
  const g = product.value;
  if (!g || busy.value) return;
  renameTried.value = true;
  const key = newKey.value.trim().toUpperCase();
  if (!KEY_RULE.test(key) || key === g.key) return;
  busy.value = true;
  const result = await updateProduct(g.id, { key });
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  renaming.value = false;
  // The row as the patch answered it, so the page finds the product by its new
  // key at its new address.
  if (data.value) data.value.products = data.value.products.map((d) => (d.id === g.id ? result.value : d));
  await router.replace(productLink(result.value));
  flash(`${g.key} is now ${key}.`);
}

// ---------------------------------------------------------------------------
// The releases.

const building = ref(false);
const buildVersion = ref('');
const buildRevision = ref(0);
const buildTried = ref(false);
const versionProblem = computed(() => (buildTried.value && !VERSION_RULE.test(buildVersion.value.trim()) ? 'A game version is one to four numbers between dots.' : ''));

useUnsavedChanges(() => {
  const g = product.value;
  const edit = editing.value;
  return !!g && (
    (!!edit && (edit.display_name !== g.display_name || edit.lobby !== g.lobby ||
      edit.updater_login !== g.updater_login || edit.command_file !== g.command_file)) ||
    (renaming.value && newKey.value.trim().toUpperCase() !== g.key) ||
    (building.value && (buildVersion.value.trim() !== '' || buildRevision.value !== g.head)) ||
    (rejecting.value !== null && rejectNote.value.trim() !== '')
  );
});

function beginBuild() {
  building.value = true;
  buildVersion.value = '';
  buildRevision.value = product.value?.head ?? 0;
  buildTried.value = false;
}

async function confirmBuild() {
  const g = product.value;
  if (!g || g.kind === 'app' || busy.value) return;
  buildTried.value = true;
  const version = buildVersion.value.trim();
  if (!VERSION_RULE.test(version)) return;
  busy.value = true;
  const result = await buildRelease(g.key, { version, revision: buildRevision.value || g.head });
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  building.value = false;
  flash(`${releaseLabel(result.value)} is building.`);
  await poll.readAgain();
}

// A draft published, or a release awaiting approval approved (published).
async function publish(r: Release) {
  if (busy.value) return;
  const name = releaseLabel(r);
  const approve = r.status === 'awaiting_approval';
  if (!await confirmAction(`${approve ? 'Approve' : 'Publish'} ${name}? Players get it and new servers run it.`)) return;
  busy.value = true;
  const result = await publishRelease(r.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  if (rejecting.value?.id === r.id) rejecting.value = null;
  flash(approve ? `${name} is approved and published.` : `${name} is published.`);
  await poll.readAgain();
}

const rejecting = ref<Release | null>(null);
const rejectNote = ref('');

function beginReject(r: Release) {
  rejecting.value = rejecting.value?.id === r.id ? null : r;
  rejectNote.value = '';
  building.value = false;
}

async function confirmReject() {
  const r = rejecting.value;
  if (!r || busy.value) return;
  const name = releaseLabel(r);
  busy.value = true;
  const result = await rejectRelease(r.id, rejectNote.value.trim());
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  rejecting.value = null;
  flash(`${name} is back with its owner as a draft.`);
  await poll.readAgain();
}

async function retire(r: Release) {
  if (busy.value) return;
  const name = releaseLabel(r);
  if (!await confirmAction(`Retire ${name}? Players stop getting it.`)) return;
  busy.value = true;
  const result = await retireRelease(r.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  flash(`${name} is retired.`);
  await poll.readAgain();
}

async function removeRelease(r: Release) {
  if (busy.value) return;
  const name = releaseLabel(r);
  if (!await confirmAction(`Delete ${name}? Its package is removed.`)) return;
  busy.value = true;
  const result = await deleteRelease(r.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  flash(`${name} is deleted.`);
  await poll.readAgain();
}

// ---------------------------------------------------------------------------
// The servers.

async function updateServers() {
  const g = product.value;
  if (!g || busy.value) return;
  const n = g.servers_outdated;
  const version = g.current_tree?.version || 'the published release';
  if (!await confirmAction(`Move ${n} server${n === 1 ? '' : 's'} of ${g.display_name} to ${version}? Each restarts and drops its players.`)) return;
  busy.value = true;
  const result = await updateAllServers(g.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  const asked = result.value;
  flash(asked === 0 ? `No server of ${g.display_name} is outdated.` : `${asked} server${asked === 1 ? ' is' : 's are'} moving to ${version}.`);
  await poll.readAgain();
}
</script>
