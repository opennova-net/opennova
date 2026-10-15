<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">CD keys</h1>
        <p class="text-sm text-ink-muted">
          Keys for the games that activate against NovaWorld: Delta Force Xtreme 2. A copy is activated on its
          first NovaWorld login, for the machine it runs on. Joint Operations needs no key.
        </p>
        <p v-if="list" class="mt-1 text-xs text-ink-muted">
          <template v-if="list.auto_mint">
            An account with no key gets one at its first activation.
          </template>
          <template v-else>Only keys assigned here activate; an account with no key is not activated.</template>
        </p>
      </div>
      <button
        type="button"
        class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40"
        :disabled="loading"
        @click="load"
      >
        {{ loading ? 'Refreshing…' : 'Refresh' }}
      </button>
    </header>

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them; the boxes below are their visible copies. -->
    <div role="status" class="sr-only">{{ message }}</div>
    <div role="alert" class="sr-only">{{ error }}</div>
    <div v-if="message" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">
      {{ message }}
    </div>
    <div v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      {{ error }}
    </div>

    <section class="space-y-4" aria-labelledby="cd-generate-heading">
      <h2 id="cd-generate-heading" class="text-xl font-semibold text-ink">Generate</h2>
      <form class="flex flex-wrap items-end gap-4 rounded-panel border border-border bg-panel p-4 sm:p-6" @submit.prevent="generate">
        <label class="space-y-1">
          <span class="block text-xs font-medium uppercase tracking-wider text-ink-muted">Game</span>
          <select
            id="cd-generate-title"
            v-model="generateTitle"
            class="rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink"
          >
            <option v-for="t in titles" :key="t" :value="t">{{ titleName(t) }}</option>
          </select>
        </label>
        <label class="space-y-1">
          <span class="block text-xs font-medium uppercase tracking-wider text-ink-muted">How many</span>
          <input
            id="cd-generate-count"
            v-model.number="generateCount"
            type="number"
            min="1"
            :max="MAX_GENERATE"
            class="w-28 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink"
          />
        </label>
        <button
          type="submit"
          :disabled="busy || !generateTitle"
          class="rounded-control bg-accent px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-accent/90 disabled:opacity-40"
        >
          Generate
        </button>
        <p v-if="generateError" id="cd-generate-error" class="w-full text-sm text-danger">{{ generateError }}</p>
      </form>
      <div v-if="generated.length" class="rounded-panel border border-online/40 bg-online/5 p-4">
        <p class="text-sm text-ink">New keys, unassigned:</p>
        <ul id="cd-generated" class="mt-2 space-y-1 font-mono text-sm text-ink">
          <li v-for="k in generated" :key="k.id">{{ k.serial }}</li>
        </ul>
      </div>
    </section>

    <section class="space-y-4" aria-labelledby="cd-keys-heading">
      <h2 id="cd-keys-heading" class="text-xl font-semibold text-ink">Keys</h2>
      <form class="flex flex-wrap items-end gap-4" @submit.prevent="applyFilter">
        <label class="space-y-1">
          <span class="block text-xs font-medium uppercase tracking-wider text-ink-muted">Game</span>
          <select id="cd-filter-title" v-model="filterTitle" class="rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink">
            <option value="">All</option>
            <option v-for="t in titles" :key="t" :value="t">{{ titleName(t) }}</option>
          </select>
        </label>
        <label class="space-y-1">
          <span class="block text-xs font-medium uppercase tracking-wider text-ink-muted">State</span>
          <select id="cd-filter-state" v-model="filterState" class="rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink">
            <option value="">All</option>
            <option value="unassigned">Unassigned</option>
            <option value="assigned">Assigned</option>
            <option value="revoked">Revoked</option>
          </select>
        </label>
        <label class="space-y-1">
          <span class="block text-xs font-medium uppercase tracking-wider text-ink-muted">Search</span>
          <input
            id="cd-filter-q"
            v-model="filterQuery"
            type="search"
            placeholder="Key or account name"
            class="w-64 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink"
          />
        </label>
        <button
          type="submit"
          class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover"
        >
          Apply
        </button>
      </form>

      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">CD keys, newest first</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">ID</th>
              <th scope="col" class="px-4 py-3">Key</th>
              <th scope="col" class="px-4 py-3">State</th>
              <th scope="col" class="px-4 py-3">Account</th>
              <th scope="col" class="px-4 py-3">Machines</th>
              <th scope="col" class="px-4 py-3">Made</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="list === null">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">{{ loading ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="list.keys.length === 0">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">No keys match.</td>
            </tr>
            <tr v-for="k in list?.keys ?? []" :key="k.id" class="cd-key-row align-top hover:bg-hover">
              <td class="px-4 py-3 font-mono text-xs">{{ k.id }}</td>
              <th scope="row" class="px-4 py-3 text-left font-normal">
                <span class="block font-mono">{{ k.serial }}</span>
                <span class="block text-xs text-ink-muted">{{ titleName(k.title) }}</span>
              </th>
              <td class="whitespace-nowrap px-4 py-3">
                <span :class="stateClass(k.state)">{{ stateLabel(k.state) }}</span>
              </td>
              <td class="px-4 py-3">
                <button
                  v-if="k.account"
                  type="button"
                  class="text-accent hover:text-ink-bright"
                  @click="openAccount(k.account.id)"
                >
                  {{ k.account.username }}<span class="sr-only">: show the account's keys and machines</span>
                </button>
                <form
                  v-else-if="k.state === 'unassigned'"
                  class="flex gap-2"
                  @submit.prevent="assign(k)"
                >
                  <label :for="`cd-assign-${k.id}`" class="sr-only">Account name for key {{ k.serial }}</label>
                  <input
                    :id="`cd-assign-${k.id}`"
                    v-model="assignNames[k.id]"
                    type="text"
                    autocomplete="off"
                    placeholder="Account name"
                    class="w-36 rounded-control border border-border bg-surface px-2 py-1 text-xs text-ink"
                  />
                  <button
                    type="submit"
                    :disabled="busy || !(assignNames[k.id] ?? '').trim()"
                    class="text-xs text-accent hover:text-ink-bright disabled:opacity-40"
                  >
                    Assign
                  </button>
                </form>
                <span v-else class="text-ink-muted">None</span>
              </td>
              <td class="px-4 py-3">
                <button type="button" class="text-accent hover:text-ink-bright" @click="openKey(k)">
                  {{ k.activations }}<span class="sr-only"> machines for key {{ k.serial }}: show them</span>
                </button>
              </td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime :value="k.created_at" :now="now" />
                <span class="block">{{ creatorLabel(k.created_by) }}</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3 text-right">
                <button
                  v-if="k.state !== 'revoked'"
                  type="button"
                  :disabled="busy"
                  class="text-danger/80 hover:text-danger disabled:opacity-40"
                  @click="revoke(k)"
                >
                  Revoke<span class="sr-only"> key {{ k.serial }}</span>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <div v-if="list && list.total > PAGE" class="flex items-center gap-3 text-sm text-ink-muted">
        <button type="button" class="text-accent disabled:opacity-40" :disabled="offset === 0" @click="page(-1)">Previous</button>
        <span>{{ offset + 1 }} to {{ Math.min(offset + PAGE, list.total) }} of {{ list.total }}</span>
        <button type="button" class="text-accent disabled:opacity-40" :disabled="offset + PAGE >= list.total" @click="page(1)">Next</button>
      </div>
    </section>

    <section
      v-if="keyPanel"
      id="cd-key-panel"
      class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6"
      aria-labelledby="cd-key-panel-heading"
    >
      <div class="flex items-start justify-between gap-4">
        <div class="min-w-0">
          <p class="text-xs uppercase tracking-[0.3em] text-ink-muted">Key #{{ keyPanel.key.id }}</p>
          <h2 id="cd-key-panel-heading" class="mt-1 break-all font-mono text-xl text-ink">{{ keyPanel.key.serial }}</h2>
          <p class="mt-1 break-all font-mono text-xs text-ink-muted">CDKIID {{ keyPanel.key.wire_key }}</p>
        </div>
        <button type="button" class="shrink-0 text-sm text-ink-muted hover:text-ink" @click="keyPanel = null">Close</button>
      </div>
      <ActivationTable :activations="keyPanel.activations" empty="This key has activated no machine." />
      <form v-if="keyPanel.key.state !== 'revoked'" class="space-y-2" @submit.prevent="activate">
        <label for="cd-activate-nwpssk" class="block text-sm text-ink">
          Activate a machine by hand: its fingerprint (NWPSSK, 23 letters, from the game's login or the server log)
        </label>
        <div class="flex flex-wrap gap-2">
          <input
            id="cd-activate-nwpssk"
            v-model="manualNwpssk"
            type="text"
            autocomplete="off"
            spellcheck="false"
            maxlength="23"
            class="w-72 rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm uppercase text-ink"
          />
          <button
            type="submit"
            :disabled="busy || !NWPSSK_SHAPE.test(manualNwpssk.trim().toUpperCase())"
            class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:opacity-40"
          >
            Get its activation code
          </button>
        </div>
        <div v-if="manualNwact" class="space-y-1">
          <p class="text-xs text-ink-muted">
            The value for that machine's registry CDKIID
            (HKLM\Software\WOW6432Node\NovaLogic\Delta Force Xtreme 2\1.00.0000):
          </p>
          <p id="cd-activate-nwact" class="break-all rounded-control border border-border bg-surface p-2 font-mono text-xs text-ink">
            {{ manualNwact }}
          </p>
        </div>
      </form>
    </section>

    <section class="space-y-4" aria-labelledby="cd-account-heading">
      <h2 id="cd-account-heading" class="text-xl font-semibold text-ink">An account's keys and machines</h2>
      <form class="flex flex-wrap items-end gap-2" @submit.prevent="lookUpAccount">
        <label for="cd-account-name" class="sr-only">Account name</label>
        <input
          id="cd-account-name"
          v-model="accountName"
          type="text"
          autocomplete="off"
          placeholder="Account name"
          class="w-64 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink"
        />
        <button
          type="submit"
          :disabled="busy || !accountName.trim()"
          class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:opacity-40"
        >
          Show
        </button>
      </form>
      <div v-if="accountPanel" id="cd-account-panel" class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6">
        <p class="text-lg font-semibold text-ink">{{ accountPanel.account.username }}</p>
        <ul class="space-y-1 text-sm">
          <li v-if="accountPanel.keys.length === 0" class="text-ink-muted">No keys.</li>
          <li v-for="k in accountPanel.keys" :key="k.id">
            <span class="font-mono">{{ k.serial }}</span>
            <span class="ml-2" :class="stateClass(k.state)">{{ stateLabel(k.state) }}</span>
          </li>
        </ul>
        <ActivationTable :activations="accountPanel.activations" empty="No machine is activated on this account." />
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/cd-keys: the CD keys (GET /api/admin/cd-keys) with their filters, the
// generate form (POST /api/admin/cd-keys), assign (by account name) and revoke
// per key, a key's machines and a manual activation (/api/admin/cd-keys/<id>/
// activations), and an account's keys and machines (/api/admin/users/<id>/cd-keys,
// the account found by name in the users list).
import { defineComponent, h, onMounted, ref, computed, type PropType } from 'vue';
import {
  activateCdKey,
  assignCdKey,
  fetchAccountCdKeys,
  fetchCdKeys,
  fetchKeyActivations,
  generateCdKeys,
  revokeCdKey,
} from '../../api/cdKeys';
import { listUsers } from '../../api/users';
import type { CdKey, CdKeyAccountResponse, CdKeyActivation, CdKeyListResponse, CdKeyState } from '../../types/cdKeys';
import RelativeTime from '../../components/RelativeTime.vue';

const MAX_GENERATE = 100;
const PAGE = 50;
const NWPSSK_SHAPE = /^[A-Z]{23}$/;
const TITLE_NAMES: Record<string, string> = { dfx2: 'Delta Force Xtreme 2' };

// A list of activations: when, the machine, its NWACT.
const ActivationTable = defineComponent({
  props: {
    activations: { type: Array as PropType<CdKeyActivation[]>, required: true },
    empty: { type: String, required: true },
  },
  setup(props) {
    return () =>
      props.activations.length === 0
        ? h('p', { class: 'text-sm text-ink-muted' }, props.empty)
        : h('div', { class: 'relative overflow-x-auto' }, [
            h('table', { class: 'cd-activations min-w-full divide-y divide-border text-sm' }, [
              h('thead', [
                h('tr', { class: 'text-left text-xs uppercase tracking-wider text-ink-muted' }, [
                  h('th', { scope: 'col', class: 'px-2 py-2' }, 'When'),
                  h('th', { scope: 'col', class: 'px-2 py-2' }, 'Key'),
                  h('th', { scope: 'col', class: 'px-2 py-2' }, 'Machine fingerprint (NWPSSK)'),
                  h('th', { scope: 'col', class: 'px-2 py-2' }, 'Activation code (NWACT)'),
                ]),
              ]),
              h(
                'tbody',
                { class: 'divide-y divide-border text-ink' },
                props.activations.map((a) =>
                  h('tr', { key: a.id, class: 'align-top' }, [
                    h('td', { class: 'whitespace-nowrap px-2 py-2 text-xs text-ink-muted' }, [h(RelativeTime, { value: a.created_at, now: Date.now() })]),
                    h('td', { class: 'whitespace-nowrap px-2 py-2 font-mono text-xs' }, a.serial),
                    h('td', { class: 'px-2 py-2 font-mono text-xs' }, a.nwpssk),
                    h('td', { class: 'max-w-md break-all px-2 py-2 font-mono text-xs text-ink-muted' }, a.nwact),
                  ]),
                ),
              ),
            ]),
          ]);
  },
});

const list = ref<CdKeyListResponse | null>(null);
// The page's clock, for the relative times.
const now = ref(Date.now());
const loading = ref(false);
const busy = ref(false);
const message = ref<string | null>(null);
const error = ref<string | null>(null);

const generateTitle = ref('dfx2');
const generateCount = ref(10);
const generateError = ref<string | null>(null);
const generated = ref<CdKey[]>([]);

const filterTitle = ref('');
const filterState = ref<CdKeyState | ''>('');
const filterQuery = ref('');
const offset = ref(0);
const assignNames = ref<Record<number, string>>({});

const keyPanel = ref<{ key: CdKey; activations: CdKeyActivation[] } | null>(null);
const manualNwpssk = ref('');
const manualNwact = ref('');
const accountName = ref('');
const accountPanel = ref<CdKeyAccountResponse | null>(null);

const titles = computed(() => list.value?.titles ?? ['dfx2']);

function titleName(code: string): string {
  return TITLE_NAMES[code] ?? code;
}

function stateLabel(state: CdKeyState): string {
  return { unassigned: 'Unassigned', assigned: 'Assigned', revoked: 'Revoked' }[state] ?? state;
}

function stateClass(state: CdKeyState): string {
  const base = 'rounded-control px-2 py-0.5 text-xs font-medium';
  if (state === 'assigned') return `${base} bg-online/10 text-online`;
  if (state === 'revoked') return `${base} bg-danger/10 text-danger`;
  return `${base} bg-raised text-ink-muted`;
}

function creatorLabel(createdBy: string): string {
  if (createdBy === 'auto') return 'at its first activation';
  if (createdBy === 'token') return 'by the admin token';
  return createdBy.startsWith('admin ') ? `by ${createdBy.slice(6)}` : createdBy;
}

function flash(text: string) {
  message.value = text;
  error.value = null;
}

function fail(text: string) {
  error.value = text;
  message.value = null;
}

async function load() {
  loading.value = true;
  now.value = Date.now();
  const result = await fetchCdKeys({
    title: filterTitle.value,
    state: filterState.value,
    q: filterQuery.value.trim(),
    limit: PAGE,
    offset: offset.value,
  });
  loading.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  list.value = result.value;
  if (!result.value.titles.includes(generateTitle.value)) generateTitle.value = result.value.titles[0] ?? '';
}

function applyFilter() {
  offset.value = 0;
  void load();
}

function page(direction: number) {
  offset.value = Math.max(0, offset.value + direction * PAGE);
  void load();
}

async function generate() {
  generateError.value = null;
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
  flash(`Generated ${result.value.length} key${result.value.length === 1 ? '' : 's'}.`);
  offset.value = 0;
  await load();
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
  const who = k.account ? ` It is on ${k.account.username}'s account.` : '';
  if (!window.confirm(`Revoke key ${k.serial}?${who} It will activate no machine again.`)) return;
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

async function openKey(k: CdKey) {
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
  flash(result.value.reactivated ? 'That machine was already activated; here is its NWACT again.' : 'The machine is activated.');
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
    await openAccount(user.id);
  } finally {
    busy.value = false;
  }
}

onMounted(() => {
  void load();
});
</script>
