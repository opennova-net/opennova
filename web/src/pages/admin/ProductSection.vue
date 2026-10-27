<template>
  <div class="space-y-10">
    <AdminPageHeader
      :title="product?.display_name ?? (missing ? 'No such product' : 'Product')"
      :description="product ? kindLine(product, products, data?.games ?? []) : ''"
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="poll.refresh"
    >
      <template v-if="product && product.kind !== 'app'" #actions>
        <button id="product-edit-toggle" type="button" :class="secondaryButton" :disabled="busy" @click="toggleEdit">
          {{ editing ? 'Close' : 'Edit…' }}
        </button>
        <button id="product-delete-toggle" type="button" :class="dangerButton" :disabled="busy" @click="toggleDelete">Delete…</button>
      </template>
    </AdminPageHeader>

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them, each its own box while it holds something: the text once. -->
    <div role="status" :class="message ? 'rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online' : 'sr-only'">{{ message }}</div>
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>

    <div v-if="missing" class="rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted">
      No product has the key or id {{ productRef }}.
      <RouterLink to="/admin/products" class="text-accent hover:text-ink-bright">See every product</RouterLink>
    </div>
    <div v-else-if="!product" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">Loading the product…</div>

    <template v-else>
      <dl v-if="product.kind !== 'app'" class="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <div class="rounded-panel border border-border bg-panel p-5"><dt class="text-xs text-ink-muted">Current server release</dt><dd class="mt-2 text-xl font-semibold text-ink">{{ product.current_tree?.version || 'None ready' }}</dd></div>
        <div class="rounded-panel border border-border bg-panel p-5"><dt class="text-xs text-ink-muted">Live servers</dt><dd class="mt-2 text-2xl font-semibold text-ink">{{ product.servers_live }}</dd></div>
        <div class="rounded-panel border border-border bg-panel p-5"><dt class="text-xs text-ink-muted">Awaiting update</dt><dd class="mt-2 text-2xl font-semibold" :class="product.servers_outdated ? 'text-warn' : 'text-ink'">{{ product.servers_outdated }}</dd></div>
      </dl>
      <nav aria-label="Product details" class="flex flex-wrap gap-2 border-b border-border pb-4 text-sm">
        <a v-if="product.kind !== 'app'" href="#product-project" class="rounded-control bg-raised px-3 py-2 text-accent">Project</a>
        <a href="#product-releases" class="rounded-control bg-raised px-3 py-2 text-accent">Releases</a>
        <a v-if="product.kind !== 'app'" href="#product-servers" class="rounded-control bg-raised px-3 py-2 text-accent">Servers</a>
      </nav>
      <!-- The product's settings. -->
      <form v-if="editing" id="product-edit-form" class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6" novalidate @submit.prevent="saveEdit">
        <h2 class="text-lg font-semibold text-ink">Edit {{ product.display_name }}</h2>
        <div class="grid gap-4 sm:grid-cols-2">
          <label class="space-y-1">
            <span :class="labelClass">Name</span>
            <input id="product-edit-display_name" v-model="editing.display_name" type="text" autocomplete="off" maxlength="64" :class="inputClass" />
          </label>
          <label v-if="product.kind === 'game'" class="space-y-1">
            <span :class="labelClass">Lobby</span>
            <select id="product-edit-lobby" v-model="editing.lobby" :class="inputClass">
              <option v-for="g in data?.games ?? []" :key="g.slug" :value="g.slug">{{ g.display_name }}</option>
            </select>
          </label>
          <label class="space-y-1">
            <span :class="labelClass">Updater login</span>
            <input id="product-edit-updater_login" v-model="editing.updater_login" type="text" autocomplete="off" spellcheck="false" maxlength="32" :class="[inputClass, 'font-mono']" />
          </label>
          <label class="space-y-1">
            <span :class="labelClass">Command file</span>
            <input id="product-edit-command_file" v-model="editing.command_file" type="text" autocomplete="off" spellcheck="false" maxlength="15" :class="[inputClass, 'font-mono']" />
          </label>
          <label class="flex items-center gap-3 text-sm text-ink sm:col-span-2">
            <input id="product-edit-retail" v-model="editing.retail" type="checkbox" class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent" />
            Retail: the update service names NovaLogic's ftp.novaworld.com as the download host, not this service's.
          </label>
        </div>
        <p v-if="editError" id="product-edit-error" class="text-xs text-danger">{{ editError }}</p>
        <div class="flex flex-wrap gap-2">
          <button type="submit" :disabled="busy" :class="primaryButton">Save</button>
          <button type="button" :disabled="busy" :class="secondaryButton" @click="editing = null">Cancel</button>
        </div>
      </form>

      <!-- Its deletion, behind its key (or name) typed. -->
      <form v-if="deleting" id="product-delete-form" class="space-y-3 rounded-panel border border-danger/40 bg-panel p-4 sm:p-6" @submit.prevent="confirmDelete">
        <p class="text-sm font-semibold text-danger">
          Delete {{ product.display_name }}? Its releases, its servers' data and its project's history and files go, and an
          editor bound to it can no longer pull or push. A game a live server runs, or an expansion builds on, stays.
        </p>
        <label for="product-delete-confirm" class="block text-sm text-ink">
          Type <span class="font-mono font-semibold">{{ confirmWord }}</span> to confirm
        </label>
        <input id="product-delete-confirm" v-model="typed" type="text" autocomplete="off" spellcheck="false" class="w-full max-w-xs rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink focus:border-danger focus:outline-none" />
        <div class="flex flex-wrap gap-2">
          <button type="submit" :disabled="busy || typed.trim() !== confirmWord" :class="dangerButton">{{ busy ? 'Deleting…' : 'Delete now' }}</button>
          <button type="button" :disabled="busy" :class="secondaryButton" @click="deleting = false">Cancel</button>
        </div>
      </form>

      <!-- The project: its revisions in the service. -->
      <section v-if="product.kind !== 'app'" class="space-y-3" aria-labelledby="product-project">
        <h2 id="product-project" class="text-xl font-semibold text-ink">Project</h2>
        <template v-if="product.key">
          <p id="product-project-line" class="text-sm text-ink">
            <span class="font-mono font-semibold">{{ product.key }}</span>,
            {{ product.visibility === 'public' ? 'public: anyone may get it.' : 'private: admins only.' }}
            <template v-if="product.head > 0">
              Newest r{{ product.head }}<template v-if="newest"> by {{ newest.author }}: <q>{{ newest.message }}</q></template>.
            </template>
            <template v-else>Nothing pushed yet: bind a project folder to {{ product.key }} in the editor and push.</template>
          </p>
          <div class="flex flex-wrap gap-2">
            <button v-if="product.head > 0" id="product-history-toggle" type="button" :class="secondaryButton" :aria-expanded="showHistory ? 'true' : 'false'" @click="showHistory = !showHistory">
              {{ showHistory ? 'Hide history' : 'History' }}
            </button>
            <button id="product-visibility" type="button" :class="secondaryButton" :disabled="busy" @click="toggleVisibility">
              {{ product.visibility === 'public' ? 'Make private' : 'Make public' }}
            </button>
            <button id="product-rename-toggle" type="button" :class="secondaryButton" :disabled="busy" @click="toggleRename">
              {{ renaming ? 'Close' : 'Rename…' }}
            </button>
          </div>
          <form v-if="renaming" id="product-rename-form" class="space-y-2" novalidate @submit.prevent="saveRename">
            <label for="product-rename-key" :class="labelClass">New key for {{ product.key }}</label>
            <div class="flex flex-wrap items-center gap-2">
              <input
                id="product-rename-key"
                v-model="newKey"
                type="text"
                autocomplete="off"
                autocapitalize="characters"
                spellcheck="false"
                maxlength="16"
                :aria-invalid="renameProblem ? 'true' : undefined"
                :class="[inputClass, 'max-w-xs font-mono']"
              />
              <button type="submit" :disabled="busy || newKey.trim().toUpperCase() === product.key" :class="primaryButton">Save key</button>
            </div>
            <p class="text-xs" :class="renameProblem ? 'text-danger' : 'text-ink-muted'">
              {{ renameProblem || `An editor bound to ${product.key} pulls and pushes by that key: bind it again to the new one.` }}
            </p>
          </form>
          <div v-if="showHistory" id="product-history" class="space-y-3">
            <p v-if="history.revisions.length === 0" class="text-sm text-ink-muted">{{ history.loading ? 'Loading…' : 'Nothing has been pushed yet.' }}</p>
            <ol v-else class="divide-y divide-border rounded-panel border border-border bg-panel">
              <li v-for="r in history.revisions" :key="r.number" class="product-revision flex flex-wrap items-baseline gap-x-3 gap-y-1 px-4 py-2 text-sm">
                <span class="font-mono text-xs font-semibold">r{{ r.number }}</span>
                <span class="min-w-0 flex-1 break-words text-ink">{{ r.message }}</span>
                <span class="text-xs text-ink-muted">
                  {{ r.author }}, {{ r.file_count }} file{{ r.file_count === 1 ? '' : 's' }}, {{ formatBytes(r.bytes) }},
                  <RelativeTime :value="r.created_at" :now="now" />
                </span>
              </li>
            </ol>
            <button v-if="history.more" type="button" :disabled="history.loading" :class="secondaryButton" @click="olderRevisions">
              {{ history.loading ? 'Loading…' : 'Older revisions' }}
            </button>
          </div>
        </template>
        <p v-else class="text-sm text-ink-muted">This product has no project.</p>
      </section>

      <!-- The releases: packed builds, published to players and servers. -->
      <section class="space-y-3" aria-labelledby="product-releases">
        <h2 id="product-releases" class="text-xl font-semibold text-ink">Releases</h2>
        <p class="text-sm text-ink-muted">
          <template v-if="product.kind === 'app'">
            Published by the Publish workflow: each OpenNova build it ships reaches the downloads and this service's update
            service at once.
          </template>
          <template v-else>
            Each is built from a revision of the project and checked, then waits as a draft. Publishing it serves it to
            players' updaters and builds what new servers run; retiring it stops the serving.
          </template>
        </p>

        <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
          <table class="min-w-full divide-y divide-border">
            <caption class="sr-only">Releases of {{ product.display_name }}, newest first</caption>
            <thead>
              <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
                <th scope="col" class="px-4 py-3">Release</th>
                <th scope="col" class="px-4 py-3">Status</th>
                <th scope="col" class="px-4 py-3">Package</th>
                <th scope="col" class="px-4 py-3">Made</th>
                <th scope="col" class="px-4 py-3 text-right">Actions</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-border text-sm text-ink">
              <tr v-if="ownReleases.length === 0">
                <td colspan="5" class="px-4 py-6 text-center text-ink-muted">No release yet.</td>
              </tr>
              <tr v-for="r in ownReleases" :key="r.id" class="product-release align-top hover:bg-hover">
                <th scope="row" class="px-4 py-3 text-left font-medium">
                  <span class="font-mono">{{ releaseLabel(r) }}</span>
                  <span v-if="r.source" class="font-normal text-ink-muted">
                    (r{{ r.source.revision }}<template v-if="r.source.base_revision">, on its base's r{{ r.source.base_revision }}</template>)
                  </span>
                  <span class="block text-xs font-normal text-ink-muted">Release {{ r.id }}</span>
                </th>
                <td class="px-4 py-3">
                  <span :class="releaseStatusClass(r.status)">{{ releaseStatusLabel(r.status) }}</span>
                  <span v-if="r.error" class="block max-w-xs whitespace-normal text-xs text-danger">{{ r.error }}</span>
                  <span v-if="treeOf(r)" class="block text-xs text-ink-muted">{{ treeStateText(treeOf(r)!) }}</span>
                  <details v-if="r.build_log" class="product-release-log mt-1 max-w-md text-xs" :open="r.status === 'building'">
                    <summary class="cursor-pointer text-accent hover:text-ink-bright">Build log</summary>
                    <pre class="mt-1 max-h-64 overflow-auto whitespace-pre-wrap break-all rounded-control border border-border bg-surface p-2 font-mono text-ink">{{ r.build_log }}</pre>
                  </details>
                  <span v-if="r.published_at" class="block text-xs text-ink-muted">published <RelativeTime :value="r.published_at" :now="now" /></span>
                  <span v-if="r.retired_at" class="block text-xs text-ink-muted">retired <RelativeTime :value="r.retired_at" :now="now" /></span>
                </td>
                <td class="px-4 py-3 text-xs">
                  <span class="block">{{ r.bytes > 0 ? formatBytes(r.bytes) : 'Not packed yet' }}</span>
                  <span v-if="r.sha256" class="block font-mono text-ink-muted">{{ r.sha256.slice(0, 12) }}</span>
                </td>
                <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                  <RelativeTime :value="r.created_at" :now="now" />
                  <span class="block">{{ creatorLabel(r.created_by) }}</span>
                </td>
                <td class="space-x-3 whitespace-nowrap px-4 py-3 text-right text-xs">
                  <button v-if="r.status === 'draft'" type="button" :disabled="busy" class="text-accent hover:text-ink-bright disabled:opacity-40" @click="publish(r)">
                    Publish<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                  </button>
                  <button v-if="r.status === 'published'" type="button" :disabled="busy" class="text-warn/90 hover:text-warn disabled:opacity-40" @click="retire(r)">
                    Retire<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                  </button>
                  <button
                    v-if="r.status === 'draft' || r.status === 'invalid' || r.status === 'retired'"
                    type="button"
                    :disabled="busy"
                    class="text-danger/80 hover:text-danger disabled:opacity-40"
                    @click="removeRelease(r)"
                  >
                    Delete<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                  </button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>

        <template v-if="product.key && product.head > 0">
          <button v-if="!building" id="product-build-toggle" type="button" :class="secondaryButton" :disabled="busy" @click="beginBuild">Release r{{ product.head }}…</button>
          <form v-else id="product-build-form" class="space-y-2 rounded-panel border border-border bg-panel p-4" novalidate @submit.prevent="confirmBuild">
            <p class="text-sm text-ink">
              The data program packs a revision of {{ product.display_name }} whole<template v-if="product.kind === 'expansion'">, on its base game's newest revision</template>,
              and checks it: a draft here once it is done.
            </p>
            <div class="flex flex-wrap items-end gap-3">
              <div class="space-y-1">
                <label for="product-build-version" :class="labelClass">Version</label>
                <input
                  id="product-build-version"
                  v-model="buildVersion"
                  type="text"
                  autocomplete="off"
                  spellcheck="false"
                  placeholder="0.2.0"
                  maxlength="32"
                  :aria-invalid="versionProblem ? 'true' : undefined"
                  :class="[inputClass, 'w-32 font-mono']"
                />
              </div>
              <div class="space-y-1">
                <label for="product-build-revision" :class="labelClass">Revision</label>
                <input id="product-build-revision" v-model.number="buildRevision" type="number" min="1" :max="product.head" :class="[inputClass, 'w-24 font-mono']" />
              </div>
              <button type="submit" :disabled="busy || !buildVersion.trim()" :class="primaryButton">{{ busy ? 'Starting…' : 'Build release' }}</button>
              <button type="button" :disabled="busy" :class="secondaryButton" @click="building = false">Cancel</button>
            </div>
            <p class="text-xs" :class="versionProblem ? 'text-danger' : 'text-ink-muted'">
              {{ versionProblem || `One to four numbers between dots. Revision ${product.head} is the newest.` }}
            </p>
          </form>
        </template>
        <p v-else-if="product.key" class="text-sm text-ink-muted">Push a revision from the editor to build a release.</p>
      </section>

      <!-- The hosted servers running it. -->
      <section v-if="product.kind !== 'app'" class="space-y-3" aria-labelledby="product-servers">
        <h2 id="product-servers" class="text-xl font-semibold text-ink">Servers</h2>
        <RouterLink :to="{ path: '/admin/fleet', query: { game: product.id } }" class="inline-block text-sm text-accent hover:text-ink">View these servers in the fleet</RouterLink>
        <p id="product-servers-line" class="text-sm text-ink">
          {{ product.servers_live }} live server{{ product.servers_live === 1 ? '' : 's' }}.
          <template v-if="product.current_tree">New servers start on {{ product.current_tree.version || 'the published release' }}.</template>
          <template v-else>No release is ready for servers yet: publish one.</template>
        </p>
        <div v-if="product.servers_outdated > 0 && product.current_tree" class="flex flex-wrap items-center gap-3 text-sm text-warn">
          <span>{{ product.servers_outdated }} run{{ product.servers_outdated === 1 ? 's' : '' }} an older release.</span>
          <button id="product-update-servers" type="button" :class="secondaryButton" :disabled="busy" @click="updateServers">
            Move {{ product.servers_outdated === 1 ? 'it' : 'them' }} to {{ product.current_tree.version || 'the published release' }}
          </button>
        </div>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
import { confirmAction } from '../../composables/useConfirm';
// /admin/products/<key or id>: one product (GET /api/admin/products, and
// GET /api/admin/releases for its releases). Its settings (PATCH) and its
// deletion behind its key typed (DELETE); its project (key, visibility, the
// newest revision and its history from GET /api/products/<key>/revisions,
// a page at a time; a visibility change or a key rename by PATCH); its releases,
// each built from a revision (POST /api/admin/products/<key>/releases),
// then published, retired or deleted; and its servers moved to the published
// release (POST /api/admin/hosted/update-all). The runtime's page has its
// releases alone. The page polls, faster while a release is being worked on.
import { computed, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import RelativeTime from '../../components/RelativeTime.vue';
import {
  REVISIONS_PAGE,
  buildRelease,
  deleteProduct,
  deleteRelease,
  fetchProducts,
  fetchReleases,
  fetchRevisions,
  publishRelease,
  retireRelease,
  updateAllServers,
  updateProduct,
} from '../../api/products';
import { usePolling } from '../../composables/usePolling';
import { useUnsavedChanges } from '../../composables/useUnsavedChanges';
import type { ProductsResponse, ProjectRevision, Release } from '../../types/products';
import { formatBytes } from '../../utils/format';
import {
  KEY_RULE,
  VERSION_RULE,
  creatorLabel,
  findProduct,
  kindLine,
  listProducts,
  productLink,
  releaseLabel,
  releaseStatusClass,
  releaseStatusLabel,
  releasesOf,
  releasesWorking,
  treeStateText,
  updaterProblems,
} from '../../utils/productsView';

const BUSY_POLL_MS = 3_000;
const IDLE_POLL_MS = 30_000;

const labelClass = 'block text-xs font-medium uppercase tracking-wider text-ink-muted';
const inputClass = 'w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none aria-[invalid=true]:border-danger';
const primaryButton = 'rounded-control bg-accent px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-40';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';
const dangerButton = 'rounded-control bg-danger px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-danger/90 disabled:cursor-not-allowed disabled:opacity-40';

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

const products = computed(() => listProducts(data.value?.products ?? [], releases.value));
const product = computed(() => findProduct(products.value, productRef.value));
const missing = computed(() => data.value !== null && product.value === null);
const ownReleases = computed(() => (product.value ? releasesOf(product.value, releases.value) : []));

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
  () => [product.value?.key ?? null, product.value?.head ?? 0] as const,
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
  showHistory.value = false;
});

// ---------------------------------------------------------------------------
// The product's settings and its deletion.

interface EditForm {
  display_name: string;
  lobby: string;
  updater_login: string;
  command_file: string;
  retail: boolean;
}

const editing = ref<EditForm | null>(null);
const editError = ref('');

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
    retail: g.retail,
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
    ...(g.kind === 'game' ? { lobby: e.lobby } : {}),
    updater_login: login,
    command_file: commandFile,
    retail: e.retail,
  });
  busy.value = false;
  if (!result.ok) {
    editError.value = result.error.message;
    return;
  }
  editing.value = null;
  flash(`${result.value.display_name} is saved.`);
  await poll.readAgain();
}

const deleting = ref(false);
const typed = ref('');
const confirmWord = computed(() => product.value?.key ?? product.value?.display_name ?? '');

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
  if (!g || !g.key || busy.value) return;
  const visibility = g.visibility === 'public' ? 'private' : 'public';
  busy.value = true;
  const result = await updateProduct(g.id, { visibility });
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  flash(visibility === 'public' ? `Anyone may now get ${g.key}.` : `Only admins may now get ${g.key}.`);
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
const versionProblem = computed(() => (buildTried.value && !VERSION_RULE.test(buildVersion.value.trim()) ? 'A version is one to four numbers between dots.' : ''));

useUnsavedChanges(() => {
  const g = product.value;
  const edit = editing.value;
  return !!g && (
    (!!edit && (edit.display_name !== g.display_name || edit.lobby !== g.lobby ||
      edit.updater_login !== g.updater_login || edit.command_file !== g.command_file || edit.retail !== g.retail)) ||
    (renaming.value && newKey.value.trim().toUpperCase() !== g.key) ||
    (building.value && (buildVersion.value.trim() !== '' || buildRevision.value !== g.head))
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
  if (!g || !g.key || busy.value) return;
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
  flash(`${releaseLabel(result.value)} is building: a draft to publish here once it is checked.`);
  await poll.readAgain();
}

async function publish(r: Release) {
  if (busy.value) return;
  const name = releaseLabel(r);
  if (!await confirmAction(`Publish ${name}? The update service serves it to players, and new servers run it once its data is built; running servers move when their owners choose.`)) return;
  busy.value = true;
  const result = await publishRelease(r.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  flash(`${name} is published.`);
  await poll.readAgain();
}

async function retire(r: Release) {
  if (busy.value) return;
  const name = releaseLabel(r);
  if (!await confirmAction(`Retire ${name}? The update service stops serving it; servers keep what they run.`)) return;
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
  if (!await confirmAction(`Delete ${name}? Its package is removed from the store.`)) return;
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
  if (!await confirmAction(`Move the ${n} server${n === 1 ? '' : 's'} of ${g.display_name} on an older release to ${version}? Each restarts, and its players drop while it does.`)) {
    return;
  }
  busy.value = true;
  const result = await updateAllServers(g.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  const asked = result.value;
  flash(asked === 0 ? `No server of ${g.display_name} runs an older release.` : `${asked} server${asked === 1 ? ' is' : 's are'} moving to ${version}.`);
  await poll.readAgain();
}
</script>
