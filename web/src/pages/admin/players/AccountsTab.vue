<template>
  <div class="space-y-6">
    <PageHeader title="Players" :tabs="PLAYERS_TABS" refreshable :loaded-at="loadedAt" :now="now" :loading="loading" @refresh="refresh">
      <template v-if="users" #meta>
        <span class="accounts-count" aria-live="polite">
          {{ filtering ? `${formatCount(matched.length)} of ${formatCount(users.length)}` : formatCount(users.length) }}
          account{{ users.length === 1 ? '' : 's' }}
        </span>
      </template>
      <template #actions>
        <button type="button" class="btn-primary" aria-controls="accounts-new" :aria-expanded="createOpen ? 'true' : 'false'" @click="createOpen = !createOpen">
          New account
        </button>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" />

    <form v-if="createOpen" id="accounts-new" class="card grid items-end gap-3 p-4 sm:grid-cols-2 lg:grid-cols-3" @submit.prevent="createUser">
      <label>
        <span class="field-label">Username</span>
        <input id="accounts-new-username" v-model="newUser.username" required autocomplete="off" class="field" />
      </label>
      <label>
        <span class="field-label">Password</span>
        <input id="accounts-new-password" v-model="newUser.password" type="password" required autocomplete="new-password" class="field" />
      </label>
      <label>
        <span class="field-label">Email</span>
        <input id="accounts-new-email" v-model="newUser.email" type="email" required autocomplete="off" class="field" />
      </label>
      <label>
        <span class="field-label">In-game handle</span>
        <input id="accounts-new-handle" v-model="newUser.nwhandle" required autocomplete="off" class="field" />
      </label>
      <label>
        <span class="field-label">PCID</span>
        <input
          id="accounts-new-pcid"
          v-model="newUser.pcid"
          autocomplete="off"
          placeholder="Made up when empty"
          title="8 hexadecimal digits"
          class="field font-mono"
        />
      </label>
      <div class="flex gap-2">
        <button type="submit" :disabled="creating" class="btn-primary">{{ creating ? 'Creating…' : 'Create account' }}</button>
        <button type="button" class="btn-ghost" @click="createOpen = false">Cancel</button>
      </div>
    </form>

    <div class="accounts-filters flex flex-wrap items-center gap-2">
      <label for="users-filter" class="sr-only">Find an account</label>
      <input
        id="users-filter"
        v-model="filter"
        type="search"
        autocomplete="off"
        placeholder="Name, email, PCID or ID"
        class="field w-auto sm:w-64"
      />
      <label for="users-role" class="sr-only">Role</label>
      <select id="users-role" v-model="roleFilter" class="field w-auto">
        <option value="">All roles</option>
        <option value="admin">Admin</option>
        <option value="player">Player</option>
      </select>
      <label for="users-status" class="sr-only">Status</label>
      <select id="users-status" v-model="statusFilter" class="field w-auto">
        <option value="">All statuses</option>
        <option v-for="status in statuses" :key="status" :value="status">{{ accountStatusLabel(status) }}</option>
      </select>
      <button v-if="filtering" type="button" class="btn-ghost btn-sm" @click="clearFilters">Clear</button>
    </div>

    <div class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">Player accounts</caption>
        <thead>
          <tr>
            <th scope="col">Account</th>
            <th scope="col">Role</th>
            <th scope="col">Support tier</th>
            <th v-if="showBilling" scope="col">Subscription</th>
            <th scope="col">Servers</th>
            <th scope="col">Last seen</th>
            <th scope="col">Online</th>
            <th scope="col">Status</th>
          </tr>
        </thead>
        <tbody>
          <TableState
            v-if="shown.length === 0"
            :colspan="showBilling ? 8 : 7"
            :rows="users"
            :loading="loading"
            :empty="users?.length ? 'No account matches.' : 'No accounts yet.'"
          />
          <tr v-for="u in shown" :key="u.id" class="user-row">
            <th scope="row">
              <PlayerLink :id="u.id" :name="u.username" /> <span v-if="isSelf(u)" class="text-xs font-normal text-ink-muted">(you)</span> <span
                v-if="u.nwhandle && u.nwhandle !== u.username"
                class="user-handle ml-1 text-xs font-normal text-ink-muted"
                title="In-game handle"
              >{{ u.nwhandle }}</span>
              <span v-if="u.email" class="block break-all text-xs font-normal text-ink-muted">{{ u.email }}</span>
            </th>
            <td><Badge :tone="u.role === 'admin' ? 'info' : 'muted'">{{ u.role === 'admin' ? 'Admin' : 'Player' }}</Badge></td>
            <td class="user-tier"><Badge :tone="tierTone(u)">{{ entitlementLabel(u.entitlement) }}</Badge></td>
            <td v-if="showBilling" class="whitespace-nowrap" :class="u.subscription_status === 'past_due' ? 'text-warn' : u.subscription_status ? '' : 'text-ink-muted'">
              {{ u.subscription_status ? subscriptionStatusLabel(u.subscription_status) : 'None' }}
            </td>
            <td class="user-servers whitespace-nowrap">
              <template v-if="u.live_servers > 0">
                <RouterLink :to="{ path: '/admin/servers', query: { owner: u.username } }" class="entity-link">{{ u.live_servers }} live</RouterLink> <span
                  v-if="u.live_servers > u.entitlement.max_servers"
                  class="text-xs text-danger"
                >over {{ u.entitlement.max_servers }}</span>
              </template>
              <span v-else class="text-ink-muted">None</span>
            </td>
            <td class="whitespace-nowrap text-ink-muted">
              <RelativeTime v-if="u.last_login" :value="u.last_login" :now="now" />
              <template v-else>Never</template>
            </td>
            <td class="user-online whitespace-nowrap">
              <span v-if="onlineIds.has(u.id)" class="inline-flex items-center gap-1.5 text-online">
                <span class="h-2 w-2 rounded-full bg-online" aria-hidden="true"></span>Online
              </span>
            </td>
            <td><Badge :tone="accountStatusTone(u.account_status)">{{ accountStatusLabel(u.account_status) }}</Badge></td>
          </tr>
        </tbody>
      </table>
    </div>

    <nav v-if="pageCount > 1" aria-label="Account pages" class="flex items-center justify-between gap-3 text-sm text-ink-muted">
      <RouterLink v-if="page > 1" :to="pageLink(page - 1)" class="btn-secondary btn-sm">Previous</RouterLink>
      <span v-else></span>
      <p>Page {{ page }} of {{ pageCount }}</p>
      <RouterLink v-if="page < pageCount" :to="pageLink(page + 1)" class="btn-secondary btn-sm">Next</RouterLink>
      <span v-else></span>
    </nav>
  </div>
</template>

<script setup lang="ts">
// /admin/players/accounts: every account (GET /api/admin/users) with its role,
// support tier, subscription, live servers, last sign-in, whether it is online
// now and its status, filtered and paged in the URL query; each opens its
// player page. And a new account (POST /api/admin/users, or the public
// registration when no PCID is given, which makes one up). The Subscription
// column shows only to the admin who sees billing (utils/billingAccess.ts).
import { computed, onMounted, reactive, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import RelativeTime from '../../../components/RelativeTime.vue';
import PlayerLink from '../../../components/links/PlayerLink.vue';
import Badge from '../../../components/ui/Badge.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TableState from '../../../components/ui/TableState.vue';
import { fetchOnline } from '../../../api/online';
import { createUserAdmin, listUsers, registerUser } from '../../../api/users';
import { useSession } from '../../../composables/useSession';
import type { UserListEntry } from '../../../types/users';
import { PLAYERS_TABS } from '../../../utils/adminNav';
import { accountStatusLabel, accountStatusTone, entitlementLabel } from '../../../utils/adminView';
import { seesBilling } from '../../../utils/billingAccess';
import { subscriptionStatusLabel } from '../../../utils/billingView';
import { formatCount } from '../../../utils/format';
import type { Tone } from '../../../utils/tone';
import { useQueryText } from '../../../composables/useQueryText';

const PAGE_SIZE = 50;

const { user: sessionUser } = useSession();
const showBilling = computed(() => seesBilling(sessionUser.value));
const route = useRoute();
const router = useRouter();

const users = ref<UserListEntry[] | null>(null);
// The accounts signed in to NovaWorld now.
const onlineIds = ref<ReadonlySet<number>>(new Set());
const loading = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const createOpen = ref(false);
const creating = ref(false);
const newUser = reactive({ username: '', password: '', pcid: '', nwhandle: '', email: '' });

const filter = useQueryText('q', ['page']);
const roleFilter = useQueryText('role', ['page']);
const statusFilter = useQueryText('status', ['page']);
const pageText = useQueryText('page');
const filtering = computed(() => !!(filter.value || roleFilter.value || statusFilter.value));
const statuses = computed(() => [...new Set((users.value ?? []).map((u) => u.account_status || 'active'))].sort());

const matched = computed(() => {
  const q = filter.value.trim().toLowerCase();
  return (users.value ?? []).filter(
    (u) =>
      (!q || [u.username, u.nwhandle, u.email, u.pcid, String(u.id)].some((value) => value?.toLowerCase().includes(q))) &&
      (!roleFilter.value || u.role === roleFilter.value) &&
      (!statusFilter.value || (u.account_status || 'active') === statusFilter.value),
  );
});
const pageCount = computed(() => Math.max(1, Math.ceil(matched.value.length / PAGE_SIZE)));
const page = computed(() => Math.min(pageCount.value, Math.max(1, Number.parseInt(pageText.value, 10) || 1)));
const shown = computed(() => matched.value.slice((page.value - 1) * PAGE_SIZE, page.value * PAGE_SIZE));

function pageLink(value: number) {
  return { path: route.path, query: { ...route.query, page: value > 1 ? String(value) : undefined } };
}

function clearFilters() {
  void router.replace({ path: route.path, query: {} });
}

function isSelf(u: UserListEntry): boolean {
  return sessionUser.value?.id === u.id;
}

// A tier's (or dedicated machines paid for with no tier) reads as info, an
// admin's grant as ok, nothing as muted.
function tierTone(u: UserListEntry): Tone {
  const e = u.entitlement;
  if (e.source === 'tier') return 'info';
  if (e.source === 'comp' || e.comp_own_machines > 0) return 'ok';
  return e.own_machines > 0 ? 'info' : 'muted';
}

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
}

async function refresh() {
  loading.value = true;
  const [result, online] = await Promise.all([listUsers(), fetchOnline()]);
  loading.value = false;
  now.value = Date.now();
  // Who is online is a nicety: its failure leaves the column empty.
  if (online.ok) onlineIds.value = new Set(online.value.players.flatMap((p) => (p.account ? [p.account.id] : [])));
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  users.value = result.value;
  loadedAt.value = now.value;
  error.value = '';
}

async function createUser() {
  if (creating.value) return;
  creating.value = true;
  const name = newUser.username.trim();
  try {
    if (newUser.pcid.trim() === '') {
      // No PCID: the public registration makes one up.
      await registerUser({ username: name, password: newUser.password, nwhandle: newUser.nwhandle.trim(), email: newUser.email.trim() });
    } else {
      const result = await createUserAdmin({
        username: name,
        password: newUser.password,
        pcid: newUser.pcid.trim(),
        nwhandle: newUser.nwhandle.trim(),
        email: newUser.email.trim(),
      });
      if (!result.ok) {
        fail(result.error.message);
        return;
      }
    }
    flash(`Created ${name}.`);
    Object.assign(newUser, { username: '', password: '', pcid: '', nwhandle: '', email: '' });
    createOpen.value = false;
    await refresh();
  } catch (e) {
    const code = (e as { response?: { data?: { error?: unknown } } })?.response?.data?.error;
    fail(
      code === 'username_exists'
        ? 'Another account already has that username.'
        : code === 'email_exists'
          ? 'Another account already uses that email address.'
          : 'The account could not be created. Check the fields and try again.',
    );
  } finally {
    creating.value = false;
  }
}

onMounted(refresh);
</script>
