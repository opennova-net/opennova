<template>
  <div class="space-y-10">
    <AdminPageHeader title="Users" description="Accounts with their support tier and hosted servers." :loading="loading" @refresh="refresh" />

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them, each its own box while it holds something: the text once. -->
    <div role="status" :class="message ? 'rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online' : 'sr-only'">{{ message }}</div>
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>

    <section class="space-y-4" aria-labelledby="users-heading">
      <div class="flex flex-col gap-3 sm:flex-row sm:items-end sm:justify-between">
        <h2 id="users-heading" class="text-xl font-semibold text-ink">Accounts</h2>
        <label class="space-y-1">
          <span class="block text-xs font-medium text-ink-muted">Find an account</span>
          <input
            id="users-filter"
            v-model="filter"
            type="search"
            autocomplete="off"
            placeholder="Name, email, PCID or account ID"
            class="w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none sm:w-64"
          />
        </label>
      </div>
      <div class="flex flex-wrap items-end gap-3 rounded-panel border border-border bg-panel p-4">
        <label class="space-y-1">
          <span class="block text-xs font-medium text-ink-muted">Role</span>
          <select id="users-role" v-model="roleFilter" class="rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink">
            <option value="">All roles</option><option value="admin">Admin</option><option value="player">Player</option>
          </select>
        </label>
        <label class="space-y-1">
          <span class="block text-xs font-medium text-ink-muted">Account status</span>
          <select id="users-status" v-model="statusFilter" class="rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink">
            <option value="">All statuses</option>
            <option v-for="status in statuses" :key="status" :value="status">{{ accountStatusLabel(status) }}</option>
          </select>
        </label>
        <p class="self-center text-sm text-ink-muted" aria-live="polite">{{ matched.length }} of {{ users?.length ?? 0 }} accounts</p>
        <button v-if="filter || roleFilter || statusFilter" type="button" class="self-center text-sm text-accent hover:text-ink" @click="clearFilters">Clear filters</button>
      </div>
      <!-- relative: the rows' sr-only spans are absolutely positioned, and without a
           positioned ancestor they escape this scroller and widen the page on a phone. -->
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Player accounts</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">Account</th>
              <th scope="col" class="px-4 py-3">Role</th>
              <th scope="col" class="px-4 py-3">Support tier</th>
              <th scope="col" class="px-4 py-3">Subscription</th>
              <th scope="col" class="px-4 py-3">Hosted servers</th>
              <th scope="col" class="px-4 py-3">Status</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="users === null">
              <td colspan="6" class="px-4 py-6 text-center text-ink-muted">{{ loading ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="shown.length === 0">
              <td colspan="6" class="px-4 py-6 text-center text-ink-muted">
                {{ users.length === 0 ? 'No accounts yet.' : 'No account matches.' }}
              </td>
            </tr>
            <tr v-for="u in shown" :key="u.id" class="user-row hover:bg-hover">
              <th scope="row" class="px-4 py-3 text-left font-medium">
                <RouterLink :to="`/admin/users/${u.id}`" class="text-accent hover:text-ink-bright">{{ u.username }}</RouterLink>
                <span v-if="isSelf(u)" class="ml-1 text-xs font-normal text-ink-muted">(you)</span>
                <span class="block text-xs font-normal text-ink-muted">Account {{ u.id }}</span>
                <span v-if="u.email" class="block break-all text-xs font-normal text-ink-muted">{{ u.email }}</span>
              </th>
              <td class="px-4 py-3"><span :class="roleBadgeClass(u.role)">{{ u.role === 'admin' ? 'Admin' : 'Player' }}</span></td>
              <td class="whitespace-nowrap px-4 py-3">
                <span :class="tierBadgeClass(u)">{{ tierLabel(u) }}</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <span :class="u.subscription_status === 'past_due' ? 'text-warn' : 'text-ink'">
                  {{ u.subscription_status ? subscriptionStatusLabel(u.subscription_status) : 'None' }}
                </span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <span :class="u.live_servers > u.entitlement.max_servers ? 'text-danger' : ''">
                  {{ u.live_servers }} live of {{ u.entitlement.max_servers }}
                </span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <span :class="(u.account_status || 'active') === 'active' ? 'text-ink' : 'text-warn'">
                  {{ accountStatusLabel(u.account_status) }}
                </span>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <nav v-if="pageCount > 1" aria-label="Account pages" class="flex items-center justify-between gap-3">
        <RouterLink v-if="page > 1" :to="pageLink(page - 1)" class="rounded-control border border-border px-4 py-2 text-sm text-ink">Previous</RouterLink>
        <span v-else class="text-sm text-ink-muted">First page</span>
        <p class="text-sm text-ink-muted">Page {{ page }} of {{ pageCount }}</p>
        <RouterLink v-if="page < pageCount" :to="pageLink(page + 1)" class="rounded-control border border-border px-4 py-2 text-sm text-ink">Next</RouterLink>
        <span v-else class="text-sm text-ink-muted">Last page</span>
      </nav>
    </section>

    <section class="space-y-4" aria-labelledby="users-create">
      <details class="rounded-panel border border-border bg-panel p-5">
      <summary id="users-create" class="cursor-pointer font-semibold text-ink">Create an account</summary>
      <form class="grid gap-4 rounded-panel border border-border bg-panel p-6 sm:grid-cols-2" @submit.prevent="createUser">
        <label class="space-y-1">
          <span :class="labelClass">Username</span>
          <input v-model="newUser.username" required autocomplete="off" :class="inputClass" />
        </label>
        <label class="space-y-1">
          <span :class="labelClass">Password</span>
          <input v-model="newUser.password" type="password" required autocomplete="new-password" :class="inputClass" />
        </label>
        <label class="space-y-1">
          <span :class="labelClass">PCID</span>
          <input v-model="newUser.pcid" autocomplete="off" aria-describedby="users-create-pcid-hint" :class="[inputClass, 'font-mono']" />
          <span id="users-create-pcid-hint" class="block text-xs text-ink-muted">
            The player's NovaWorld id, 8 hexadecimal digits. Leave it empty and one is made up.
          </span>
        </label>
        <label class="space-y-1">
          <span :class="labelClass">NovaWorld handle</span>
          <input v-model="newUser.nwhandle" required autocomplete="off" aria-describedby="users-create-handle-hint" :class="inputClass" />
          <span id="users-create-handle-hint" class="block text-xs text-ink-muted">The player's name in the game.</span>
        </label>
        <label class="space-y-1">
          <span :class="labelClass">Email</span>
          <input v-model="newUser.email" type="email" required autocomplete="off" aria-describedby="users-create-email-hint" :class="inputClass" />
          <span id="users-create-email-hint" class="block text-xs text-ink-muted">
            The player's address. Made without a PCID, the account gets the welcome mail.
          </span>
        </label>
        <div class="flex justify-end sm:col-span-2">
          <button type="submit" :disabled="creating" :class="primaryButton">{{ creating ? 'Creating…' : 'Create account' }}</button>
        </div>
      </form>
      </details>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/users: every player account (GET /api/admin/users) with its role,
// support tier, subscription status and hosted servers live against its
// entitlement; each opens its own page (/admin/users/<id>, UserDetailSection)
// for the edits. And the admin's account creation (POST /api/admin/users, or
// the public registration when no PCID is given, which makes one up).
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import { computed, onMounted, reactive, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import { createUserAdmin, listUsers, registerUser } from '../../api/users';
import { useSession } from '../../composables/useSession';
import type { Role, UserListEntry } from '../../types/users';
import { accountStatusLabel, entitlementLabel } from '../../utils/adminView';
import { subscriptionStatusLabel } from '../../utils/billingView';

const labelClass = 'block text-xs font-medium uppercase tracking-wider text-ink-muted';
const inputClass =
  'w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none';
const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:opacity-50';

const { user: sessionUser } = useSession();

const users = ref<UserListEntry[] | null>(null);
const loading = ref(false);
const creating = ref(false);
const message = ref('');
const error = ref('');
const route = useRoute();
const router = useRouter();
function queryText(key: string) { return typeof route.query[key] === 'string' ? route.query[key] as string : ''; }
function setFilter(key: string, value: string) {
  void router.replace({ path: route.path, query: { ...route.query, [key]: value || undefined, page: undefined } });
}
const filter = computed({ get: () => queryText('q'), set: (value: string) => setFilter('q', value) });
const roleFilter = computed({ get: () => queryText('role'), set: (value: string) => setFilter('role', value) });
const statusFilter = computed({ get: () => queryText('status'), set: (value: string) => setFilter('status', value) });
const statuses = computed(() => [...new Set((users.value ?? []).map((u) => u.account_status || 'active'))].sort());
const pageCount = computed(() => Math.max(1, Math.ceil(matched.value.length / 50)));
const page = computed(() => Math.min(pageCount.value, Math.max(1, Number.parseInt(queryText('page'), 10) || 1)));
function pageLink(value: number) { return { path: route.path, query: { ...route.query, page: value > 1 ? String(value) : undefined } }; }
function clearFilters() { void router.replace({ path: route.path, query: {} }); }

const newUser = reactive({ username: '', password: '', pcid: '', nwhandle: '', email: '' });

// Filtering and paging stay local: billing, keys and role checks still receive every account.
const matched = computed(() => {
  const q = filter.value.trim().toLowerCase();
  return (users.value ?? []).filter((u) =>
    (!q || [u.username, u.nwhandle, u.email, u.pcid, String(u.id)].some((value) => value?.toLowerCase().includes(q))) &&
    (!roleFilter.value || u.role === roleFilter.value) &&
    (!statusFilter.value || (u.account_status || 'active') === statusFilter.value),
  );
});
const shown = computed(() => matched.value.slice((page.value - 1) * 50, page.value * 50));

function isSelf(u: UserListEntry): boolean {
  return sessionUser.value?.id === u.id;
}

const badge = 'inline-flex rounded-control px-2 py-0.5 text-xs font-semibold';

function roleBadgeClass(role: Role): string {
  return role === 'admin' ? `${badge} bg-accent/15 text-accent` : `${badge} bg-hover text-ink-muted`;
}

function tierLabel(u: UserListEntry): string {
  return entitlementLabel(u.entitlement);
}

// The badge's colour: a tier's (or dedicated machines paid for with no tier),
// an admin's grant's, or none.
function tierBadgeClass(u: UserListEntry): string {
  const e = u.entitlement;
  if (e.source === 'tier') return `${badge} bg-accent/15 text-accent`;
  if (e.source === 'comp' || e.comp_own_machines > 0) return `${badge} bg-online/15 text-online`;
  if (e.own_machines > 0) return `${badge} bg-accent/15 text-accent`;
  return `${badge} bg-hover text-ink-muted`;
}

function flashMessage(text: string) {
  message.value = text;
  error.value = '';
}

function flashError(text: string) {
  error.value = text;
  message.value = '';
}

async function refresh() {
  loading.value = true;
  const result = await listUsers();
  loading.value = false;
  if (!result.ok) {
    flashError(result.error.message);
    return;
  }
  users.value = result.value;
}

async function createUser() {
  if (creating.value) return;
  creating.value = true;
  const name = newUser.username.trim();
  try {
    if (newUser.pcid.trim() === '') {
      // No PCID: the public registration makes one up.
      await registerUser({
        username: name,
        password: newUser.password,
        nwhandle: newUser.nwhandle.trim(),
        email: newUser.email.trim(),
      });
    } else {
      const result = await createUserAdmin({
        username: name,
        password: newUser.password,
        pcid: newUser.pcid.trim(),
        nwhandle: newUser.nwhandle.trim(),
        email: newUser.email.trim(),
      });
      if (!result.ok) {
        flashError(result.error.message);
        return;
      }
    }
    flashMessage(`Created ${name}.`);
    Object.assign(newUser, { username: '', password: '', pcid: '', nwhandle: '', email: '' });
    await refresh();
  } catch (e) {
    const code = (e as { response?: { data?: { error?: unknown } } })?.response?.data?.error;
    flashError(
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
