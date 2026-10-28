<template>
  <div class="space-y-8">
    <PageHeader title="Products" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template #actions>
        <button
          id="products-new-toggle"
          type="button"
          class="btn-primary"
          :disabled="!data"
          :aria-expanded="creating ? 'true' : 'false'"
          aria-controls="products-new-form"
          @click="toggleNewForm"
        >
          New game
        </button>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" :poll-trouble="pollTrouble" />

    <form v-if="creating && data" id="products-new-form" class="card space-y-4 p-4 sm:p-6" novalidate @submit.prevent="create">
      <h2 class="section-heading">New game</h2>
      <div class="grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
        <div>
          <label for="products-new-kind" class="field-label">Kind</label>
          <select id="products-new-kind" v-model="newForm.kind" class="field">
            <option value="game">Game</option>
            <option value="expansion">Expansion</option>
          </select>
        </div>
        <div>
          <label for="products-new-display_name" class="field-label">Name</label>
          <input
            id="products-new-display_name"
            v-model="newForm.display_name"
            type="text"
            autocomplete="off"
            maxlength="64"
            class="field"
            :aria-invalid="newErrors.display_name ? 'true' : undefined"
          />
          <p v-if="newErrors.display_name" id="products-new-display_name-error" class="mt-1 text-xs text-danger">{{ newErrors.display_name }}</p>
        </div>
        <div>
          <label for="products-new-key" class="field-label">Project key</label>
          <input
            id="products-new-key"
            v-model="newForm.key"
            type="text"
            autocomplete="off"
            autocapitalize="characters"
            spellcheck="false"
            maxlength="16"
            placeholder="JO"
            title="1 to 16 of A-Z, 0-9 and _"
            class="field font-mono"
            :aria-invalid="newErrors.key ? 'true' : undefined"
          />
          <p v-if="newErrors.key" id="products-new-key-error" class="mt-1 text-xs text-danger">{{ newErrors.key }}</p>
        </div>
        <div>
          <label for="products-new-visibility" class="field-label">Who gets the project</label>
          <select id="products-new-visibility" v-model="newForm.visibility" class="field">
            <option value="private">Admins only</option>
            <option value="public">Anyone</option>
          </select>
        </div>
        <template v-if="newForm.kind === 'game'">
          <div>
            <label for="products-new-title" class="field-label">Title</label>
            <select id="products-new-title" v-model="newForm.title" class="field" title="The game its servers speak">
              <option v-for="t in data.titles" :key="t" :value="t">{{ titleName(t) }} ({{ t }})</option>
            </select>
            <p v-if="newErrors.title" class="mt-1 text-xs text-danger">{{ newErrors.title }}</p>
          </div>
          <div>
            <label for="products-new-lobby" class="field-label">Lobby</label>
            <select id="products-new-lobby" v-model="newForm.lobby" class="field" title="The server browser its servers list in">
              <option v-for="g in data.games" :key="g.slug" :value="g.slug">{{ g.display_name }} ({{ g.slug }})</option>
            </select>
            <p v-if="newErrors.lobby" class="mt-1 text-xs text-danger">{{ newErrors.lobby }}</p>
          </div>
        </template>
        <template v-else>
          <div>
            <label for="products-new-parent_id" class="field-label">Game</label>
            <select id="products-new-parent_id" v-model.number="newForm.parent_id" class="field">
              <option v-for="b in bases" :key="b.id" :value="b.id">{{ b.display_name }}</option>
            </select>
            <p v-if="newErrors.parent_id" class="mt-1 text-xs text-danger">{{ newErrors.parent_id }}</p>
          </div>
          <div>
            <label for="products-new-expansion_code" class="field-label">Code</label>
            <input
              id="products-new-expansion_code"
              v-model="newForm.expansion_code"
              type="text"
              autocomplete="off"
              autocapitalize="none"
              spellcheck="false"
              placeholder="jox01"
              title="The /exp code"
              maxlength="8"
              class="field font-mono"
              :aria-invalid="newErrors.expansion_code ? 'true' : undefined"
            />
            <p v-if="newErrors.expansion_code" id="products-new-expansion_code-error" class="mt-1 text-xs text-danger">{{ newErrors.expansion_code }}</p>
          </div>
        </template>
        <div>
          <label for="products-new-updater_login" class="field-label">Updater login (optional)</label>
          <input
            id="products-new-updater_login"
            v-model="newForm.updater_login"
            type="text"
            autocomplete="off"
            spellcheck="false"
            placeholder="JOTRus"
            title="The login retail UPDATE.EXE sends"
            maxlength="32"
            class="field font-mono"
            :aria-invalid="newErrors.updater_login ? 'true' : undefined"
          />
          <p v-if="newErrors.updater_login" id="products-new-updater_login-error" class="mt-1 text-xs text-danger">{{ newErrors.updater_login }}</p>
        </div>
        <div>
          <label for="products-new-command_file" class="field-label">Command file</label>
          <input
            id="products-new-command_file"
            v-model="newForm.command_file"
            type="text"
            autocomplete="off"
            spellcheck="false"
            placeholder="JOPTR.CMD"
            maxlength="15"
            class="field font-mono"
            :aria-invalid="newErrors.command_file ? 'true' : undefined"
          />
          <p v-if="newErrors.command_file" id="products-new-command_file-error" class="mt-1 text-xs text-danger">{{ newErrors.command_file }}</p>
        </div>
        <label class="flex items-center gap-3 self-end text-sm text-ink">
          <input id="products-new-retail" v-model="newForm.retail" type="checkbox" class="h-4 w-4" />
          Retail (NovaLogic's download host)
        </label>
      </div>
      <div class="flex flex-wrap gap-2">
        <button id="products-new-submit" type="submit" class="btn-primary" :disabled="busy">Create</button>
        <button type="button" class="btn-secondary" :disabled="busy" @click="creating = false">Cancel</button>
      </div>
    </form>

    <section v-if="awaiting.length" class="space-y-3" aria-labelledby="products-awaiting">
      <h2 id="products-awaiting" class="section-heading">Awaiting approval</h2>
      <div class="table-wrap">
        <table class="data-table">
          <thead>
            <tr>
              <th scope="col">Product</th>
              <th scope="col">Version</th>
              <th scope="col">Owner</th>
              <th scope="col">Submitted</th>
              <th scope="col"><span class="sr-only">Actions</span></th>
            </tr>
          </thead>
          <tbody>
            <template v-for="a in awaiting" :key="a.release.id">
              <tr class="products-awaiting-row">
                <th scope="row">
                  <ProductLink v-if="a.product" :product-ref="a.product.key || a.product.id" :name="a.product.display_name" />
                  <span v-else class="text-ink-muted">Unknown product</span>
                </th>
                <td class="whitespace-nowrap font-mono">{{ releaseLabel(a.release) }}</td>
                <td class="whitespace-nowrap">
                  <PlayerLink v-if="a.product?.owner" :id="a.product.owner_id" :name="a.product.owner" />
                  <span v-else>OpenNova</span>
                </td>
                <td class="whitespace-nowrap text-ink-muted"><RelativeTime :value="a.release.created_at" :now="now" /></td>
                <td class="whitespace-nowrap text-right">
                  <button type="button" class="btn-primary btn-sm" :disabled="busy" @click="approve(a.release)">
                    Approve<span class="sr-only">{{ ' ' + (releaseLabel(a.release)) }}</span>
                  </button>
                  <button type="button" class="btn-ghost btn-sm ml-2" :disabled="busy" @click="beginReject(a.release)">
                    Reject…<span class="sr-only">{{ ' ' + (releaseLabel(a.release)) }}</span>
                  </button>
                </td>
              </tr>
              <tr v-if="rejecting === a.release.id">
                <td colspan="5">
                  <form id="products-reject-form" class="flex flex-wrap items-end gap-2" @submit.prevent="confirmReject(a.release)">
                    <div class="min-w-0 flex-1">
                      <label for="products-reject-note" class="field-label">Note to {{ a.product?.owner ?? 'its owner' }} (optional)</label>
                      <input id="products-reject-note" v-model="rejectNote" type="text" autocomplete="off" class="field" :disabled="busy" />
                    </div>
                    <button type="submit" class="btn-warn btn-sm" :disabled="busy">Send back</button>
                    <button type="button" class="btn-ghost btn-sm" :disabled="busy" @click="rejecting = null">Cancel</button>
                  </form>
                </td>
              </tr>
            </template>
          </tbody>
        </table>
      </div>
    </section>

    <div class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">Products</caption>
        <thead>
          <tr>
            <th scope="col">Product</th>
            <th scope="col">Kind</th>
            <th scope="col">Owner</th>
            <th scope="col">Published</th>
            <th scope="col">Servers</th>
          </tr>
        </thead>
        <tbody>
          <TableState v-if="products.length === 0" :colspan="5" :rows="data ? products : null" :loading="poll.loading.value" empty="No products yet." />
          <tr v-for="g in products" :key="g.id" class="products-row">
            <th scope="row">
              <ProductLink :product-ref="g.key || g.id" :name="g.display_name" />
              <span v-if="g.key" class="block font-mono text-xs font-normal text-ink-muted">{{ g.key }}</span>
            </th>
            <td class="products-kind text-ink-muted">{{ kindLine(g, products, data?.games ?? []) }}</td>
            <td class="products-owner whitespace-nowrap">
              <PlayerLink v-if="g.owner" :id="g.owner_id" :name="g.owner" />
              <span v-else>OpenNova</span>
            </td>
            <td class="whitespace-nowrap">
              <span v-if="g.published_release" class="font-mono">{{ publishedLabel(g, releases) }}</span>
              <span v-else class="text-ink-muted">None</span>
            </td>
            <td class="products-servers whitespace-nowrap">
              <template v-if="g.kind !== 'app'">
                <RouterLink :to="{ path: '/admin/servers', query: { product: g.id } }" class="entity-link">{{ g.servers_live }} live</RouterLink>
                <span v-if="g.servers_outdated > 0" class="block text-xs text-warn">{{ g.servers_outdated }} outdated</span>
              </template>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<script setup lang="ts">
// /admin/products: the releases awaiting approval (approve or send back), every
// product with its owner, published release and servers, and a new game or
// expansion of NovaWorld's own with its project.
import { computed, reactive, ref } from 'vue';
import { RouterLink, useRouter } from 'vue-router';
import RelativeTime from '../../../components/RelativeTime.vue';
import PlayerLink from '../../../components/links/PlayerLink.vue';
import ProductLink from '../../../components/links/ProductLink.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TableState from '../../../components/ui/TableState.vue';
import { createProduct, fetchProducts, fetchReleases, publishRelease, rejectRelease } from '../../../api/products';
import { confirmAction } from '../../../composables/useConfirm';
import { usePolling } from '../../../composables/usePolling';
import type { NewProduct, ProductsResponse, Release } from '../../../types/products';
import { titleName } from '../../../utils/games';
import {
  KEY_RULE,
  awaitingApproval,
  kindLine,
  productLink,
  publishedLabel,
  releaseLabel,
  releaseProductId,
  releasesWorking,
  updaterProblems,
} from '../../../utils/productsView';

const BUSY_POLL_MS = 3_000;
const IDLE_POLL_MS = 30_000;
const EXPANSION_CODE = /^[a-z0-9]{1,8}$/;

const router = useRouter();
const data = ref<ProductsResponse | null>(null);
const releases = ref<Release[]>([]);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
let errorFromLoad = false;

const products = computed(() => data.value?.products ?? []);
const bases = computed(() => products.value.filter((g) => g.kind === 'game'));
// Every release waiting for an admin, with its product.
const awaiting = computed(() =>
  awaitingApproval(releases.value).map((r) => ({ release: r, product: products.value.find((p) => p.id === releaseProductId(r)) ?? null })),
);

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
  next: () => (releasesWorking(releases.value) ? BUSY_POLL_MS : IDLE_POLL_MS),
});

// ---------------------------------------------------------------------------
// Approvals.

const rejecting = ref<number | null>(null);
const rejectNote = ref('');

async function approve(r: Release) {
  if (busy.value) return;
  const name = releaseLabel(r);
  if (!await confirmAction(`Approve ${name}? Players get it and new servers run it.`)) return;
  busy.value = true;
  const result = await publishRelease(r.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  if (rejecting.value === r.id) rejecting.value = null;
  flash(`${name} is approved and published.`);
  await poll.readAgain();
}

function beginReject(r: Release) {
  rejecting.value = rejecting.value === r.id ? null : r.id;
  rejectNote.value = '';
}

async function confirmReject(r: Release) {
  if (busy.value) return;
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
