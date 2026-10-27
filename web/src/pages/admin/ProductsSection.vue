<template>
  <div class="space-y-10">
    <AdminPageHeader
      title="Products"
      description="Each game, expansion and app: its project and its releases. Publishing a release is what players update to and what new servers run."
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="poll.refresh"
    >
      <template #actions>
        <button id="products-new-toggle" type="button" :class="secondaryButton" :disabled="!data" @click="toggleNewForm">
          {{ creating ? 'Close the form' : 'New game…' }}
        </button>
      </template>
    </AdminPageHeader>

    <!-- A live region, always in the page so a screen reader hears what lands
         in it, its own box while it holds something. -->
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>

    <form v-if="creating && data" id="products-new-form" class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6" novalidate @submit.prevent="create">
      <h2 class="text-lg font-semibold text-ink">New game</h2>
      <p class="text-sm text-ink-muted">
        A game and its empty project: an admin's editor binds a project folder to its key and pushes revision 1. A retail
        game's revision 1 is a fresh import of the stock install.
      </p>
      <div class="grid gap-4 sm:grid-cols-2">
        <label class="space-y-1">
          <span :class="labelClass">Kind</span>
          <select id="products-new-kind" v-model="newForm.kind" :class="inputClass">
            <option value="game">Game</option>
            <option value="expansion">Expansion</option>
          </select>
        </label>
        <div class="space-y-1">
          <label for="products-new-display_name" :class="labelClass">Name</label>
          <input id="products-new-display_name" v-model="newForm.display_name" type="text" autocomplete="off" maxlength="64" :class="inputClass" :aria-invalid="newErrors.display_name ? 'true' : undefined" />
          <p v-if="newErrors.display_name" id="products-new-display_name-error" class="text-xs text-danger">{{ newErrors.display_name }}</p>
        </div>
        <div class="space-y-1">
          <label for="products-new-key" :class="labelClass">Project key</label>
          <input
            id="products-new-key"
            v-model="newForm.key"
            type="text"
            autocomplete="off"
            autocapitalize="characters"
            spellcheck="false"
            maxlength="16"
            placeholder="JO"
            :class="[inputClass, 'font-mono']"
            :aria-invalid="newErrors.key ? 'true' : undefined"
          />
          <p class="text-xs" :class="newErrors.key ? 'text-danger' : 'text-ink-muted'">{{ newErrors.key || 'Up to 16 of A-Z, 0-9 and _: what an editor binds to.' }}</p>
        </div>
        <label class="space-y-1">
          <span :class="labelClass">Who gets the project</span>
          <select id="products-new-visibility" v-model="newForm.visibility" :class="inputClass">
            <option value="private">Admins only</option>
            <option value="public">Anyone</option>
          </select>
        </label>
        <template v-if="newForm.kind === 'game'">
          <div class="space-y-1">
            <label for="products-new-title" :class="labelClass">Title</label>
            <select id="products-new-title" v-model="newForm.title" :class="inputClass">
              <option v-for="t in data.titles" :key="t" :value="t">{{ titleName(t) }} ({{ t }})</option>
            </select>
            <p class="text-xs text-ink-muted">The game the server speaks: its protocol, its limits.</p>
            <p v-if="newErrors.title" class="text-xs text-danger">{{ newErrors.title }}</p>
          </div>
          <div class="space-y-1">
            <label for="products-new-lobby" :class="labelClass">Lobby</label>
            <select id="products-new-lobby" v-model="newForm.lobby" :class="inputClass">
              <option v-for="g in data.games" :key="g.slug" :value="g.slug">{{ g.display_name }} ({{ g.slug }})</option>
            </select>
            <p class="text-xs text-ink-muted">The server browser its servers list in.</p>
            <p v-if="newErrors.lobby" class="text-xs text-danger">{{ newErrors.lobby }}</p>
          </div>
        </template>
        <template v-else>
          <div class="space-y-1">
            <label for="products-new-parent_id" :class="labelClass">Game</label>
            <select id="products-new-parent_id" v-model.number="newForm.parent_id" :class="inputClass">
              <option v-for="b in bases" :key="b.id" :value="b.id">{{ b.display_name }}</option>
            </select>
            <p v-if="newErrors.parent_id" class="text-xs text-danger">{{ newErrors.parent_id }}</p>
          </div>
          <div class="space-y-1">
            <label for="products-new-expansion_code" :class="labelClass">Code</label>
            <input id="products-new-expansion_code" v-model="newForm.expansion_code" type="text" autocomplete="off" autocapitalize="none" spellcheck="false" placeholder="jox01" maxlength="8" :class="[inputClass, 'font-mono']" :aria-invalid="newErrors.expansion_code ? 'true' : undefined" />
            <p class="text-xs text-ink-muted">The /exp code: its folder under expansion/, and its two archives' name.</p>
            <p v-if="newErrors.expansion_code" id="products-new-expansion_code-error" class="text-xs text-danger">{{ newErrors.expansion_code }}</p>
          </div>
        </template>
        <div class="space-y-1">
          <label for="products-new-updater_login" :class="labelClass">Updater login (optional)</label>
          <input id="products-new-updater_login" v-model="newForm.updater_login" type="text" autocomplete="off" spellcheck="false" placeholder="JOTRus" maxlength="32" :class="[inputClass, 'font-mono']" :aria-invalid="newErrors.updater_login ? 'true' : undefined" />
          <p class="text-xs text-ink-muted">The login retail UPDATE.EXE sends for this product. With a command file, the update service serves it.</p>
          <p v-if="newErrors.updater_login" id="products-new-updater_login-error" class="text-xs text-danger">{{ newErrors.updater_login }}</p>
        </div>
        <div class="space-y-1">
          <label for="products-new-command_file" :class="labelClass">Command file</label>
          <input id="products-new-command_file" v-model="newForm.command_file" type="text" autocomplete="off" spellcheck="false" placeholder="JOPTR.CMD" maxlength="15" :class="[inputClass, 'font-mono']" :aria-invalid="newErrors.command_file ? 'true' : undefined" />
          <p class="text-xs text-ink-muted">The .CMD name a release's section carries for this product.</p>
          <p v-if="newErrors.command_file" id="products-new-command_file-error" class="text-xs text-danger">{{ newErrors.command_file }}</p>
        </div>
        <label class="flex items-center gap-3 text-sm text-ink sm:col-span-2">
          <input id="products-new-retail" v-model="newForm.retail" type="checkbox" class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent" />
          Retail: the update service names NovaLogic's ftp.novaworld.com as the download host, not this service's.
        </label>
      </div>
      <div class="flex flex-wrap gap-2">
        <button id="products-new-submit" type="submit" :disabled="busy" :class="primaryButton">Create</button>
        <button type="button" :disabled="busy" :class="secondaryButton" @click="creating = false">Cancel</button>
      </div>
    </form>

    <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
      <table class="min-w-full divide-y divide-border">
        <caption class="sr-only">Products</caption>
        <thead>
          <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
            <th scope="col" class="px-4 py-3">Product</th>
            <th scope="col" class="px-4 py-3">Project</th>
            <th scope="col" class="px-4 py-3">Published</th>
            <th scope="col" class="px-4 py-3">Servers</th>
          </tr>
        </thead>
        <tbody class="divide-y divide-border text-sm text-ink">
          <tr v-if="data === null">
            <td colspan="4" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
          </tr>
          <tr v-else-if="products.length === 0">
            <td colspan="4" class="px-4 py-6 text-center text-ink-muted">No product yet.</td>
          </tr>
          <tr v-for="g in products" :key="g.id" class="products-row align-top hover:bg-hover">
            <th scope="row" class="px-4 py-3 text-left font-medium">
              <RouterLink :to="productLink(g)" class="text-accent hover:text-ink-bright">{{ g.display_name }}</RouterLink>
              <span class="block text-xs font-normal text-ink-muted">{{ kindLine(g, products, data?.games ?? []) }}</span>
            </th>
            <td class="whitespace-nowrap px-4 py-3">
              <template v-if="g.key">
                <span class="block font-mono text-xs">{{ g.key }}</span>
                <span class="block">{{ g.visibility === 'public' ? 'Public' : 'Private' }}, {{ g.head > 0 ? `r${g.head}` : 'nothing pushed' }}</span>
              </template>
              <span v-else-if="g.kind === 'app'" class="text-ink-muted">From CI</span>
              <span v-else class="text-ink-muted">No project</span>
            </td>
            <td class="whitespace-nowrap px-4 py-3">
              <span v-if="g.published_release" class="font-mono">{{ publishedLabel(g, releases) }}</span>
              <span v-else class="text-ink-muted">None</span>
            </td>
            <td class="whitespace-nowrap px-4 py-3">
              <template v-if="g.kind !== 'app'">
                {{ g.servers_live }} live
                <span v-if="g.servers_outdated > 0" class="block text-xs text-warn">{{ g.servers_outdated }} on an older release</span>
              </template>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<script setup lang="ts">
// /admin/products: every product (GET /api/admin/products, with the runtime
// GET /api/admin/releases names) with its project, its published release and
// its servers, each linking to its page (ProductSection.vue); and a new game
// or expansion with its project (POST /api/admin/products with a key and
// visibility).
import { computed, reactive, ref } from 'vue';
import { RouterLink, useRouter } from 'vue-router';
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import { createProduct, fetchProducts, fetchReleases } from '../../api/products';
import { usePolling } from '../../composables/usePolling';
import type { ProductsResponse, NewProduct, Release } from '../../types/products';
import { KEY_RULE, kindLine, listProducts, productLink, publishedLabel, releasesWorking, titleName, updaterProblems } from '../../utils/productsView';

const BUSY_POLL_MS = 3_000;
const IDLE_POLL_MS = 30_000;
const EXPANSION_CODE = /^[a-z0-9]{1,8}$/;

const labelClass = 'block text-xs font-medium uppercase tracking-wider text-ink-muted';
const inputClass = 'w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none aria-[invalid=true]:border-danger';
const primaryButton = 'rounded-control bg-accent px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-40';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const router = useRouter();
const data = ref<ProductsResponse | null>(null);
const releases = ref<Release[]>([]);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const error = ref('');
const pollTrouble = ref(false);
let errorFromLoad = false;

const products = computed(() => listProducts(data.value?.products ?? [], releases.value));
const bases = computed(() => products.value.filter((g) => g.kind === 'game'));

function fail(text: string) {
  error.value = text;
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
  next: () => (releasesWorking(releases.value) ? BUSY_POLL_MS : IDLE_POLL_MS),
});

// ---------------------------------------------------------------------------
// The new game.

interface NewForm {
  kind: 'game' | 'expansion';
  display_name: string;
  key: string;
  visibility: 'public' | 'private';
  title: string;
  lobby: string;
  parent_id: number;
  expansion_code: string;
  updater_login: string;
  command_file: string;
  retail: boolean;
}

const creating = ref(false);
const newForm = reactive<NewForm>({
  kind: 'game',
  display_name: '',
  key: '',
  visibility: 'private',
  title: '',
  lobby: '',
  parent_id: 0,
  expansion_code: '',
  updater_login: '',
  command_file: '',
  retail: false,
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
    kind: 'game',
    display_name: '',
    key: '',
    visibility: 'private',
    title: d.titles[0] ?? '',
    lobby: d.games[0]?.slug ?? '',
    parent_id: bases.value[0]?.id ?? 0,
    expansion_code: '',
    updater_login: '',
    command_file: '',
    retail: false,
  });
  newErrors.value = {};
  creating.value = true;
}

// The form against the routes' rules; the body to send when it passes.
function checkNewForm(): NewProduct | null {
  const errors: Partial<Record<keyof NewForm, string>> = {};
  const displayName = newForm.display_name.trim();
  if (displayName === '') errors.display_name = 'Enter a name.';
  // A lower-case letter typed is its capital.
  const key = newForm.key.trim().toUpperCase();
  if (!KEY_RULE.test(key)) errors.key = 'A key is 1 to 16 of A-Z, 0-9 and _.';
  const login = newForm.updater_login.trim();
  const commandFile = newForm.command_file.trim().toUpperCase();
  Object.assign(errors, updaterProblems(login, commandFile));
  const body: NewProduct = { display_name: displayName, kind: newForm.kind, key, visibility: newForm.visibility, retail: newForm.retail };
  if (login !== '') {
    body.updater_login = login;
    body.command_file = commandFile;
  }
  if (newForm.kind === 'game') {
    if (!data.value?.titles.includes(newForm.title)) errors.title = 'Pick the title the game speaks.';
    if (!data.value?.games.some((g) => g.slug === newForm.lobby)) errors.lobby = 'Pick the lobby it lists in.';
    body.title = newForm.title;
    body.lobby = newForm.lobby;
  } else {
    const code = newForm.expansion_code.trim().toLowerCase();
    if (!bases.value.some((b) => b.id === newForm.parent_id)) errors.parent_id = 'Pick the game it builds on.';
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
  const result = await createProduct(body);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  creating.value = false;
  await router.push(productLink(result.value));
}
</script>
