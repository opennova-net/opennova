<template>
  <AdminGate>
    <div class="mx-auto max-w-6xl px-6 py-10 space-y-10">
      <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
        <div>
          <h1 class="text-3xl font-semibold text-white">Users</h1>
          <p class="text-sm text-slate-300">Create, edit, and remove player accounts.</p>
        </div>
        <div class="flex gap-2">
          <button
            type="button"
            class="rounded-md border border-white/10 bg-white/5 px-4 py-2 text-sm font-medium text-slate-50 transition hover:bg-white/10 disabled:cursor-not-allowed disabled:opacity-40"
            :disabled="loading"
            @click="refresh"
          >
            Refresh
          </button>
          <button
            type="button"
            class="rounded-md border border-rose-400/40 bg-rose-500/10 px-4 py-2 text-sm font-medium text-rose-200 transition hover:bg-rose-500/20"
            @click="signOut"
          >
            Sign out
          </button>
        </div>
      </header>

      <div v-if="message" class="rounded-md border border-emerald-400/40 bg-emerald-500/10 p-4 text-sm text-emerald-200">
        {{ message }}
      </div>
      <div v-if="error" class="rounded-md border border-rose-400/40 bg-rose-500/10 p-4 text-sm text-rose-200">
        {{ error }}
      </div>

      <section class="space-y-4">
        <h2 class="text-xl font-semibold text-white">Existing users</h2>
        <div class="overflow-hidden rounded-xl border border-white/10 bg-white/5">
          <table class="min-w-full divide-y divide-white/10">
            <thead>
              <tr class="bg-white/5 text-left text-xs font-semibold uppercase tracking-wider text-slate-300">
                <th class="px-4 py-3">ID</th>
                <th class="px-4 py-3">Username</th>
                <th class="px-4 py-3">PCID</th>
                <th class="px-4 py-3">NW Handle</th>
                <th class="px-4 py-3">NWH</th>
                <th class="px-4 py-3 text-right">Actions</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-white/5 text-sm text-slate-200">
              <tr v-if="loading">
                <td colspan="6" class="px-4 py-6 text-center text-slate-400">Loading…</td>
              </tr>
              <tr v-else-if="users.length === 0">
                <td colspan="6" class="px-4 py-6 text-center text-slate-400">No users yet.</td>
              </tr>
              <tr v-for="u in users" :key="u.id" class="hover:bg-white/5 align-top">
                <td class="px-4 py-3 font-mono text-xs">{{ u.id }}</td>
                <template v-if="editing[u.id]">
                  <td class="px-4 py-3"><input v-model="edits[u.id].username" class="w-full rounded bg-slate-900/60 px-2 py-1 text-xs"/></td>
                  <td class="px-4 py-3"><input v-model="edits[u.id].pcid"     class="w-full rounded bg-slate-900/60 px-2 py-1 font-mono text-xs"/></td>
                  <td class="px-4 py-3"><input v-model="edits[u.id].nwhandle" class="w-full rounded bg-slate-900/60 px-2 py-1 text-xs"/></td>
                  <td class="px-4 py-3"><input v-model="edits[u.id].nwh"      class="w-16 rounded bg-slate-900/60 px-2 py-1 text-xs"/></td>
                  <td class="px-4 py-3 text-right space-x-2 whitespace-nowrap">
                    <input
                      v-model="edits[u.id].password"
                      type="password"
                      placeholder="(new password)"
                      class="rounded bg-slate-900/60 px-2 py-1 text-xs w-32"
                    />
                    <button class="text-emerald-300 hover:text-emerald-200" @click="save(u.id)">Save</button>
                    <button class="text-slate-400 hover:text-slate-200" @click="cancelEdit(u.id)">Cancel</button>
                  </td>
                </template>
                <template v-else>
                  <td class="px-4 py-3">{{ u.username }}</td>
                  <td class="px-4 py-3 font-mono text-xs">{{ u.pcid }}</td>
                  <td class="px-4 py-3">{{ u.nwhandle }}</td>
                  <td class="px-4 py-3 font-mono text-xs">{{ u.nwh }}</td>
                  <td class="px-4 py-3 text-right space-x-3 whitespace-nowrap">
                    <button class="text-sky-300 hover:text-sky-200"   @click="startEdit(u)">Edit</button>
                    <button class="text-rose-300 hover:text-rose-200" @click="remove(u.id)">Delete</button>
                  </td>
                </template>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section class="space-y-4">
        <h2 class="text-xl font-semibold text-white">Create new user</h2>
        <form
          class="grid gap-4 rounded-xl border border-white/10 bg-white/5 p-6 sm:grid-cols-2"
          @submit.prevent="createUser"
        >
          <label class="space-y-1">
            <span class="text-xs font-medium uppercase tracking-wider text-slate-400">Username</span>
            <input v-model="newUser.username" required class="w-full rounded-md border border-white/10 bg-slate-900/60 px-3 py-2 text-sm text-white"/>
          </label>
          <label class="space-y-1">
            <span class="text-xs font-medium uppercase tracking-wider text-slate-400">Password</span>
            <input v-model="newUser.password" type="password" required class="w-full rounded-md border border-white/10 bg-slate-900/60 px-3 py-2 text-sm text-white"/>
          </label>
          <label class="space-y-1">
            <span class="text-xs font-medium uppercase tracking-wider text-slate-400">PCID (8 hex)</span>
            <input v-model="newUser.pcid" placeholder="auto-generate if blank" class="w-full rounded-md border border-white/10 bg-slate-900/60 px-3 py-2 font-mono text-sm text-white"/>
          </label>
          <label class="space-y-1">
            <span class="text-xs font-medium uppercase tracking-wider text-slate-400">NW Handle</span>
            <input v-model="newUser.nwhandle" required class="w-full rounded-md border border-white/10 bg-slate-900/60 px-3 py-2 text-sm text-white"/>
          </label>
          <div class="sm:col-span-2 flex justify-end">
            <button type="submit" class="rounded-md bg-emerald-500 px-4 py-2 text-sm font-medium text-white hover:bg-emerald-400">
              Create user
            </button>
          </div>
        </form>
      </section>
    </div>
  </AdminGate>
</template>

<script setup lang="ts">
import { onMounted, reactive, ref } from 'vue';
import AdminGate from '../components/AdminGate.vue';
import { clearAdminToken } from '../api/authToken';
import {
  createUserAdmin,
  deleteUser,
  listUsers,
  registerUser,
  updateUser,
} from '../api/users';
import type { User } from '../types/users';

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

function signOut() {
  clearAdminToken();
  window.location.reload();
}

onMounted(refresh);
</script>
