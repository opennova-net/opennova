<template>
  <div class="space-y-6">
    <PageHeader title="Players" :tabs="PLAYERS_TABS" refreshable :loaded-at="loadedAt" :now="now" :loading="loading" @refresh="load">
      <template v-if="list" #meta>
        <span class="cd-total">{{ formatCount(list.total) }} key{{ list.total === 1 ? '' : 's' }}</span>
        <span class="cd-mint" :title="list.auto_mint ? 'An account with no key gets one at its first activation' : 'Only keys assigned here activate'">
          Auto-mint {{ list.auto_mint ? 'on' : 'off' }}
        </span>
      </template>
      <template #actions>
        <button type="button" class="btn-primary" aria-controls="cd-generate" :aria-expanded="generateOpen ? 'true' : 'false'" @click="generateOpen = !generateOpen">
          Generate keys
        </button>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" />

    <form v-if="generateOpen" id="cd-generate" class="card flex flex-wrap items-end gap-3 p-4" @submit.prevent="generate">
      <label>
        <span class="field-label">Game</span>
        <select id="cd-generate-title" v-model="generateTitle" class="field w-auto">
          <option v-for="t in titles" :key="t" :value="t">{{ titleName(t) }}</option>
        </select>
      </label>
      <label>
        <span class="field-label">How many</span>
        <input id="cd-generate-count" v-model.number="generateCount" type="number" min="1" :max="MAX_GENERATE" class="field w-24" />
      </label>
      <button type="submit" :disabled="busy || !generateTitle" class="btn-primary">Generate</button>
      <p v-if="generateError" id="cd-generate-error" class="w-full text-sm text-danger">{{ generateError }}</p>
    </form>
    <Notice v-if="generated.length" tone="ok">
      New keys:
      <ul id="cd-generated" class="mt-1 font-mono">
        <li v-for="k in generated" :key="k.id">{{ k.serial }}</li>
      </ul>
    </Notice>

    <form class="cd-filters flex flex-wrap items-center gap-2" @submit.prevent="applySearch">
      <label for="cd-filter-title" class="sr-only">Game</label>
      <select id="cd-filter-title" v-model="filterTitle" class="field w-auto">
        <option value="">All games</option>
        <option v-for="t in titles" :key="t" :value="t">{{ titleName(t) }}</option>
      </select>
      <label for="cd-filter-state" class="sr-only">State</label>
      <select id="cd-filter-state" v-model="filterState" class="field w-auto">
        <option value="">All states</option>
        <option value="unassigned">Unassigned</option>
        <option value="assigned">Assigned</option>
        <option value="revoked">Revoked</option>
      </select>
      <label for="cd-filter-q" class="sr-only">Find a key</label>
      <input id="cd-filter-q" v-model="searchDraft" type="search" autocomplete="off" placeholder="Key or account" class="field w-auto sm:w-64" />
      <button type="submit" class="btn-secondary">Search</button>
    </form>

    <div class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">CD keys, newest first</caption>
        <thead>
          <tr>
            <th scope="col">Key</th>
            <th scope="col">State</th>
            <th scope="col">Account</th>
            <th scope="col">Machines</th>
            <th scope="col">Made</th>
            <th scope="col"><span class="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          <TableState v-if="!list || list.keys.length === 0" :colspan="6" :rows="list?.keys ?? null" :loading="loading" empty="No keys match." />
          <template v-for="k in list?.keys ?? []" :key="k.id">
            <tr class="cd-key-row">
              <th scope="row" class="font-normal">
                <span class="block font-mono">{{ k.serial }}</span>
                <span class="block text-xs text-ink-muted">{{ titleName(k.title) }}</span>
              </th>
              <td><Badge :tone="keyStateTone(k.state)">{{ keyStateLabel(k.state) }}</Badge></td>
              <td>
                <PlayerLink v-if="k.account" :id="k.account.id" :name="k.account.username" />
                <form v-else-if="k.state === 'unassigned'" class="flex gap-2" @submit.prevent="assign(k)">
                  <label :for="`cd-assign-${k.id}`" class="sr-only">Account name for key {{ k.serial }}</label>
                  <input
                    :id="`cd-assign-${k.id}`"
                    v-model="assignNames[k.id]"
                    type="text"
                    autocomplete="off"
                    placeholder="Account name"
                    class="field w-36 py-1 text-xs"
                  />
                  <button type="submit" :disabled="busy || !(assignNames[k.id] ?? '').trim()" class="btn-ghost btn-sm">Assign</button>
                </form>
                <span v-else class="text-ink-muted">None</span>
              </td>
              <td>
                <button
                  type="button"
                  class="cd-machines btn-ghost btn-sm"
                  :aria-expanded="keyPanel?.key.id === k.id ? 'true' : 'false'"
                  @click="toggleKey(k)"
                >
                  {{ k.activations }}<span class="sr-only"> machines for key {{ k.serial }}</span>
                </button>
              </td>
              <td class="whitespace-nowrap text-xs text-ink-muted">
                <RelativeTime :value="k.created_at" :now="now" />
                <span class="block">{{ keyCreatorLabel(k.created_by) }}</span>
              </td>
              <td class="text-right">
                <button v-if="k.state !== 'revoked'" type="button" :disabled="busy" class="btn-ghost btn-sm" @click="revoke(k)">
                  Revoke<span class="sr-only"> key {{ k.serial }}</span>
                </button>
              </td>
            </tr>
            <tr v-if="keyPanel && keyPanel.key.id === k.id" class="cd-key-detail">
              <td colspan="6">
                <div id="cd-key-panel" class="space-y-3">
                  <p class="text-xs text-ink-muted">CDKIID <span class="break-all font-mono text-ink">{{ keyPanel.key.wire_key }}</span></p>
                  <CdKeyActivations :activations="keyPanel.activations" :now="now" empty="This key has activated no machine." />
                  <form v-if="keyPanel.key.state !== 'revoked'" class="flex flex-wrap items-end gap-2" @submit.prevent="activate">
                    <label>
                      <span class="field-label">Activate a machine (NWPSSK)</span>
                      <input
                        id="cd-activate-nwpssk"
                        v-model="manualNwpssk"
                        type="text"
                        autocomplete="off"
                        spellcheck="false"
                        maxlength="23"
                        class="field w-72 font-mono uppercase"
                      />
                    </label>
                    <button type="submit" :disabled="busy || !NWPSSK_SHAPE.test(manualNwpssk.trim().toUpperCase())" class="btn-secondary">
                      Get its NWACT
                    </button>
                  </form>
                  <p v-if="manualNwact" class="text-sm">
                    <span class="field-label">NWACT</span>
                    <code id="cd-activate-nwact" class="break-all font-mono">{{ manualNwact }}</code>
                  </p>
                </div>
              </td>
            </tr>
          </template>
        </tbody>
      </table>
    </div>

    <nav v-if="list && list.total > PAGE" aria-label="Key pages" class="flex items-center gap-3 text-sm text-ink-muted">
      <button type="button" class="btn-secondary btn-sm" :disabled="offset === 0" @click="goPage(-1)">Previous</button>
      <span>{{ offset + 1 }} to {{ Math.min(offset + PAGE, list.total) }} of {{ list.total }}</span>
      <button type="button" class="btn-secondary btn-sm" :disabled="offset + PAGE >= list.total" @click="goPage(1)">Next</button>
    </nav>

    <section class="space-y-3" aria-labelledby="cd-account-heading">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <h2 id="cd-account-heading" class="section-heading">An account's keys</h2>
        <form class="flex gap-2" @submit.prevent="lookUpAccount">
          <label for="cd-account-name" class="sr-only">Account name</label>
          <input id="cd-account-name" v-model="accountName" type="text" autocomplete="off" placeholder="Account name" class="field w-auto" />
          <button type="submit" :disabled="busy || !accountName.trim()" class="btn-secondary">Show</button>
        </form>
      </div>
      <div v-if="accountPanel" id="cd-account-panel" class="space-y-3">
        <PlayerLink :id="accountPanel.account.id" :name="accountPanel.account.username" />
        <p v-if="accountPanel.keys.length === 0" class="text-sm text-ink-muted">No keys.</p>
        <ul v-else class="flex flex-wrap gap-x-4 gap-y-1 text-sm">
          <li v-for="k in accountPanel.keys" :key="k.id" class="flex items-center gap-2">
            <span class="font-mono">{{ k.serial }}</span>
            <Badge :tone="keyStateTone(k.state)">{{ keyStateLabel(k.state) }}</Badge>
          </li>
        </ul>
        <CdKeyActivations :activations="accountPanel.activations" :now="now" empty="No machine is activated on this account." />
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/players/cd-keys: the CD keys (GET /api/admin/cd-keys), filtered and
// paged in the URL query; generating them, assigning one by account name,
// revoking one, a key's machines with a manual activation, and an account's
// keys and machines (?account=<id>, or found by name).
import { computed, onMounted, ref, watch } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import RelativeTime from '../../../components/RelativeTime.vue';
import PlayerLink from '../../../components/links/PlayerLink.vue';
import Badge from '../../../components/ui/Badge.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import Notice from '../../../components/ui/Notice.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TableState from '../../../components/ui/TableState.vue';
import {
  activateCdKey,
  assignCdKey,
  fetchAccountCdKeys,
  fetchCdKeys,
  fetchKeyActivations,
  generateCdKeys,
  revokeCdKey,
} from '../../../api/cdKeys';
import { listUsers } from '../../../api/users';
import { confirmAction } from '../../../composables/useConfirm';
import type { CdKey, CdKeyAccountResponse, CdKeyActivation, CdKeyListResponse, CdKeyState } from '../../../types/cdKeys';
import { PLAYERS_TABS } from '../../../utils/adminNav';
import { formatCount } from '../../../utils/format';
import { titleName } from '../../../utils/games';
import CdKeyActivations from './CdKeyActivations.vue';
import { keyCreatorLabel, keyStateLabel, keyStateTone } from './cdKeyView';
import { useQueryText } from '../../../composables/useQueryText';

const MAX_GENERATE = 100;
const PAGE = 50;
const NWPSSK_SHAPE = /^[A-Z]{23}$/;

const route = useRoute();
const router = useRouter();

const list = ref<CdKeyListResponse | null>(null);
const now = ref(Date.now());
const loadedAt = ref<number | null>(null);
const loading = ref(false);
const busy = ref(false);
const message = ref('');
const error = ref('');

const generateOpen = ref(false);
const generateTitle = ref('dfx2');
const generateCount = ref(10);
const generateError = ref('');
const generated = ref<CdKey[]>([]);

const filterTitle = useQueryText('game', ['page']);
const filterState = useQueryText('state', ['page']);
const search = useQueryText('q', ['page']);
const pageText = useQueryText('page');
const searchDraft = ref(search.value);
const offset = computed(() => (Math.max(1, Number.parseInt(pageText.value, 10) || 1) - 1) * PAGE);
const assignNames = ref<Record<number, string>>({});

const keyPanel = ref<{ key: CdKey; activations: CdKeyActivation[] } | null>(null);
const manualNwpssk = ref('');
const manualNwact = ref('');
const accountName = ref('');
const accountPanel = ref<CdKeyAccountResponse | null>(null);

const titles = computed(() => list.value?.titles ?? ['dfx2']);

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
}

async function load() {
  loading.value = true;
  const result = await fetchCdKeys({
    title: filterTitle.value,
    state: filterState.value as CdKeyState | '',
    q: search.value.trim(),
    limit: PAGE,
    offset: offset.value,
  });
  loading.value = false;
  now.value = Date.now();
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  list.value = result.value;
  loadedAt.value = now.value;
  if (!result.value.titles.includes(generateTitle.value)) generateTitle.value = result.value.titles[0] ?? '';
}

// The search box applies on Search (the selects at once); the same search
// again reads again.
function applySearch() {
  const q = searchDraft.value.trim();
  if (q === search.value) void load();
  else search.value = q;
}

function goPage(direction: number) {
  const next = offset.value / PAGE + 1 + direction;
  pageText.value = next > 1 ? String(next) : '';
}

async function generate() {
  generateError.value = '';
  const count = Number(generateCount.value);
  if (!Number.isInteger(count) || count < 1 || count > MAX_GENERATE) {
    generateError.value = `Enter a whole number from 1 to ${MAX_GENERATE}.`;
    return;
  }
  busy.value = true;
  const result = await generateCdKeys(generateTitle.value, count);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  generated.value = result.value;
  generateOpen.value = false;
  flash(`Generated ${result.value.length} key${result.value.length === 1 ? '' : 's'}.`);
  if (offset.value === 0) await load();
  else pageText.value = '';
}

async function assign(k: CdKey) {
  const name = (assignNames.value[k.id] ?? '').trim();
  if (!name || busy.value) return;
  busy.value = true;
  const result = await assignCdKey(k.id, name);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  delete assignNames.value[k.id];
  flash(`Key ${k.serial} is now on ${name}'s account.`);
  await load();
}

async function revoke(k: CdKey) {
  if (busy.value) return;
  const whose = k.account ? ` (${k.account.username}'s)` : '';
  if (!(await confirmAction(`Revoke key ${k.serial}${whose}? It never activates a machine again.`))) return;
  busy.value = true;
  const result = await revokeCdKey(k.id);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  flash(`Key ${k.serial} is revoked.`);
  if (keyPanel.value?.key.id === k.id) keyPanel.value = { ...keyPanel.value, key: result.value };
  await load();
}

// A key's machines open under its row; a second click closes them.
async function toggleKey(k: CdKey) {
  if (keyPanel.value?.key.id === k.id) {
    keyPanel.value = null;
    return;
  }
  manualNwpssk.value = '';
  manualNwact.value = '';
  const result = await fetchKeyActivations(k.id);
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  keyPanel.value = { key: k, activations: result.value };
}

async function activate() {
  const panel = keyPanel.value;
  const nwpssk = manualNwpssk.value.trim().toUpperCase();
  if (!panel || !NWPSSK_SHAPE.test(nwpssk) || busy.value) return;
  busy.value = true;
  const result = await activateCdKey(panel.key.id, nwpssk);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  manualNwact.value = result.value.nwact;
  flash(result.value.reactivated ? 'That machine was already activated.' : 'The machine is activated.');
  const refreshed = await fetchKeyActivations(panel.key.id);
  if (refreshed.ok) keyPanel.value = { key: result.value.key, activations: refreshed.value };
  await load();
}

async function openAccount(userId: number) {
  const result = await fetchAccountCdKeys(userId);
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  accountPanel.value = result.value;
  accountName.value = result.value.account.username;
}

async function lookUpAccount() {
  const name = accountName.value.trim();
  if (!name) return;
  busy.value = true;
  try {
    const users = await listUsers();
    if (!users.ok) {
      fail(users.error.message);
      return;
    }
    const user = users.value.find((u) => u.username.toLowerCase() === name.toLowerCase());
    if (!user) {
      accountPanel.value = null;
      fail(`No player account is named ${name}.`);
      return;
    }
    if (route.query.account === String(user.id)) await openAccount(user.id);
    else await router.replace({ path: route.path, query: { ...route.query, account: String(user.id) } });
  } finally {
    busy.value = false;
  }
}

watch(search, (q) => {
  searchDraft.value = q;
});
watch([filterTitle, filterState, search, pageText], () => {
  void load();
});
// ?account=<id>: a player page's link, or a lookup by name.
watch(
  () => route.query.account,
  (account) => {
    if (typeof account === 'string' && /^\d+$/.test(account) && Number(account) > 0) void openAccount(Number(account));
  },
  { immediate: true },
);
onMounted(() => {
  void load();
});
</script>
