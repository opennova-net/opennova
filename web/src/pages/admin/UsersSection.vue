<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Users</h1>
        <p class="text-sm text-ink-muted">Create, edit, and remove player accounts.</p>
      </div>
      <button
        type="button"
        class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40"
        :disabled="loading"
        @click="refresh"
      >
        Refresh
      </button>
    </header>

    <div v-if="message" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">
      {{ message }}
    </div>
    <div v-if="error" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      {{ error }}
    </div>

    <section class="space-y-4">
      <h2 class="text-xl font-semibold text-ink">Existing users</h2>
      <div class="overflow-hidden rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th class="px-4 py-3">ID</th>
              <th class="px-4 py-3">Username</th>
              <th class="px-4 py-3">PCID</th>
              <th class="px-4 py-3">NW Handle</th>
              <th class="px-4 py-3">NWH</th>
              <th class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="loading">
              <td colspan="6" class="px-4 py-6 text-center text-ink-muted">Loading…</td>
            </tr>
            <tr v-else-if="users.length === 0">
              <td colspan="6" class="px-4 py-6 text-center text-ink-muted">No users yet.</td>
            </tr>
            <tr v-for="u in users" :key="u.id" class="hover:bg-hover align-top">
              <td class="px-4 py-3 font-mono text-xs">{{ u.id }}</td>
              <template v-if="editing[u.id]">
                <td class="px-4 py-3"><input v-model="edits[u.id].username" class="w-full rounded-control border border-border bg-surface px-2 py-1 text-xs text-ink"/></td>
                <td class="px-4 py-3"><input v-model="edits[u.id].pcid"     class="w-full rounded-control border border-border bg-surface px-2 py-1 font-mono text-xs text-ink"/></td>
                <td class="px-4 py-3"><input v-model="edits[u.id].nwhandle" class="w-full rounded-control border border-border bg-surface px-2 py-1 text-xs text-ink"/></td>
                <td class="px-4 py-3"><input v-model="edits[u.id].nwh"      class="w-16 rounded-control border border-border bg-surface px-2 py-1 text-xs text-ink"/></td>
                <td class="px-4 py-3 text-right space-x-2 whitespace-nowrap">
                  <input
                    v-model="edits[u.id].password"
                    type="password"
                    placeholder="(new password)"
                    class="rounded-control border border-border bg-surface px-2 py-1 text-xs text-ink w-32"
                  />
                  <button class="text-online/80 hover:text-online" @click="save(u.id)">Save</button>
                  <button class="text-ink-muted hover:text-ink" @click="cancelEdit(u.id)">Cancel</button>
                </td>
              </template>
              <template v-else>
                <td class="px-4 py-3">{{ u.username }}</td>
                <td class="px-4 py-3 font-mono text-xs">{{ u.pcid }}</td>
                <td class="px-4 py-3">{{ u.nwhandle }}</td>
                <td class="px-4 py-3 font-mono text-xs">{{ u.nwh }}</td>
                <td class="px-4 py-3 text-right space-x-3 whitespace-nowrap">
                  <button class="text-ink hover:text-ink-bright"   @click="startEdit(u)">Edit</button>
                  <button class="text-danger/80 hover:text-danger" @click="remove(u.id)">Delete</button>
                </td>
              </template>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <section class="space-y-4">
      <h2 class="text-xl font-semibold text-ink">Create new user</h2>
      <form
        class="grid gap-4 rounded-panel border border-border bg-panel p-6 sm:grid-cols-2"
        @submit.prevent="createUser"
      >
        <label class="space-y-1">
          <span class="text-xs font-medium uppercase tracking-wider text-ink-muted">Username</span>
          <input v-model="newUser.username" required class="w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink"/>
        </label>
        <label class="space-y-1">
          <span class="text-xs font-medium uppercase tracking-wider text-ink-muted">Password</span>
          <input v-model="newUser.password" type="password" required class="w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink"/>
        </label>
        <label class="space-y-1">
          <span class="text-xs font-medium uppercase tracking-wider text-ink-muted">PCID (8 hex)</span>
          <input v-model="newUser.pcid" placeholder="auto-generate if blank" class="w-full rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink"/>
        </label>
        <label class="space-y-1">
          <span class="text-xs font-medium uppercase tracking-wider text-ink-muted">NW Handle</span>
          <input v-model="newUser.nwhandle" required class="w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink"/>
        </label>
        <div class="sm:col-span-2 flex justify-end">
          <button type="submit" class="rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent hover:bg-accent/90">
            Create user
          </button>
        </div>
      </form>
    </section>
  </div>
</template>

<script setup lang="ts">
import { onMounted, reactive, ref } from 'vue';
import {
  createUserAdmin,
  deleteUser,
  listUsers,
  registerUser,
  updateUser,
} from '../../api/users';
import type { User } from '../../types/users';

const users = ref<User[]>([]);
const loading = ref(false);
const message = ref<string | null>(null);
const error = ref<string | null>(null);

const editing = reactive<Record<number, boolean>>({});
const edits   = reactive<Record<number, {
  username: string;
  pcid: string;
  nwhandle: string;
  nwh: string;
  password: string;
}>>({});

const newUser = reactive({
  username: '',
  password: '',
  pcid: '',
  nwhandle: '',
});

function flashError(e: unknown) {
  const detail = (e as { response?: { data?: { error?: string; message?: string } } })
    ?.response?.data;
  error.value = detail?.message || detail?.error || (e as Error).message || 'Unknown error';
  message.value = null;
  setTimeout(() => { error.value = null; }, 6000);
}

function flashMessage(m: string) {
  message.value = m;
  error.value = null;
  setTimeout(() => { message.value = null; }, 4000);
}

async function refresh() {
  loading.value = true;
  try {
    users.value = await listUsers();
  } catch (e) {
    flashError(e);
  } finally {
    loading.value = false;
  }
}

function startEdit(u: User) {
  editing[u.id] = true;
  edits[u.id] = {
    username: u.username,
    pcid:     u.pcid,
    nwhandle: u.nwhandle,
    nwh:      u.nwh,
    password: '',
  };
}
function cancelEdit(id: number) {
  editing[id] = false;
  delete edits[id];
}
async function save(id: number) {
  const e = edits[id];
  try {
    const payload: Record<string, string> = {
      username: e.username,
      pcid:     e.pcid,
      nwhandle: e.nwhandle,
      nwh:      e.nwh,
    };
    if (e.password) payload.password = e.password;
    await updateUser(id, payload);
    flashMessage('User updated.');
    cancelEdit(id);
    await refresh();
  } catch (err) {
    flashError(err);
  }
}

async function remove(id: number) {
  if (!confirm(`Delete user ${id}?`)) return;
  try {
    await deleteUser(id);
    flashMessage('User deleted.');
    await refresh();
  } catch (e) {
    flashError(e);
  }
}

async function createUser() {
  try {
    if (newUser.pcid.trim() === '') {
      // No PCID supplied → public registration endpoint auto-generates one.
      await registerUser({
        username: newUser.username,
        password: newUser.password,
        nwhandle: newUser.nwhandle,
      });
    } else {
      await createUserAdmin({
        username: newUser.username,
        password: newUser.password,
        pcid:     newUser.pcid,
        nwhandle: newUser.nwhandle,
      });
    }
    flashMessage(`Created '${newUser.username}'.`);
    Object.assign(newUser, { username: '', password: '', pcid: '', nwhandle: '' });
    await refresh();
  } catch (e) {
    flashError(e);
  }
}

onMounted(refresh);
</script>
