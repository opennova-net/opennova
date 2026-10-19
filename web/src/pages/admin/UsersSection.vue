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
          <span class="block text-xs font-medium text-ink-muted">Find by username</span>
          <input
            id="users-filter"
            v-model="filter"
            type="search"
            autocomplete="off"
            class="w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none sm:w-64"
          />
        </label>
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
    </section>

    <section class="space-y-4" aria-labelledby="users-create">
      <h2 id="users-create" class="text-xl font-semibold text-ink">Create an account</h2>
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
import { RouterLink } from 'vue-router';
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
const filter = ref('');

const newUser = reactive({ username: '', password: '', pcid: '', nwhandle: '', email: '' });

const shown = computed(() => {
  const all = users.value ?? [];
  const q = filter.value.trim().toLowerCase();
  return q ? all.filter((u) => u.username.toLowerCase().includes(q)) : all;
});

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

function tierBadgeClass(u: UserListEntry): string {
  if (u.entitlement.source === 'tier') return `${badge} bg-accent/15 text-accent`;
  if (u.entitlement.source === 'comp') return `${badge} bg-online/15 text-online`;
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
