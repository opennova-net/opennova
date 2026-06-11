<template>
  <section class="px-6 pt-24 pb-16">
    <div class="mx-auto max-w-3xl rounded-3xl border border-white/5 bg-white/5 p-8 shadow-2xl">
      <h1 class="text-3xl font-semibold text-white">Register</h1>
      <p class="mt-2 text-sm text-slate-400">
        Pick a handle and a password. That is what you log in with in-game.
      </p>
      <form class="mt-8 grid gap-6" @submit.prevent="handleSubmit">
        <label class="grid gap-2 text-sm text-slate-200">
          Game Handle
          <input v-model="form.username" type="text" required minlength="2" maxlength="32"
                 class="rounded-2xl border border-white/10 bg-slate-900/40 px-4 py-3 focus:border-brand-300 focus:outline-none" />
        </label>
        <label class="grid gap-2 text-sm text-slate-200">
          Display Name (NW Handle)
          <input v-model="form.nwhandle" type="text" maxlength="32"
                 placeholder="defaults to handle"
                 class="rounded-2xl border border-white/10 bg-slate-900/40 px-4 py-3 focus:border-brand-300 focus:outline-none" />
        </label>
        <label class="grid gap-2 text-sm text-slate-200">
          Password
          <input v-model="form.password" type="password" required minlength="3" maxlength="72"
                 class="rounded-2xl border border-white/10 bg-slate-900/40 px-4 py-3 focus:border-brand-300 focus:outline-none" />
        </label>
        <button type="submit" :disabled="busy"
                class="rounded-2xl bg-brand-500 px-6 py-3 font-semibold text-white hover:bg-brand-700 disabled:opacity-50">
          {{ busy ? 'Creating account…' : 'Register Account' }}
        </button>
        <p v-if="success" class="rounded-lg border border-emerald-400/40 bg-emerald-500/10 p-3 text-sm text-emerald-200">
          Account created! Username <code class="font-mono">{{ success.username }}</code>, PCID
          <code class="font-mono">{{ success.pcid }}</code>. You can now log in via Joint Operations.
        </p>
        <p v-if="error" class="rounded-lg border border-rose-400/40 bg-rose-500/10 p-3 text-sm text-rose-200">
          {{ error }}
        </p>
      </form>
    </div>
  </section>
</template>

<script setup lang="ts">
import { reactive, ref } from 'vue';
import { registerUser } from '../api/users';
import type { User } from '../types/users';

const form = reactive({ username: '', nwhandle: '', password: '' });
const busy = ref(false);
const success = ref<User | null>(null);
const error = ref<string | null>(null);

async function handleSubmit() {
  busy.value = true;
  error.value = null;
  success.value = null;
  try {
    success.value = await registerUser({
      username: form.username.trim(),
      password: form.password,
      nwhandle: form.nwhandle.trim() || undefined,
    });
    form.username = '';
    form.nwhandle = '';
    form.password = '';
  } catch (e) {
    const detail = (e as { response?: { data?: { error?: string; message?: string } } })
      ?.response?.data;
    error.value = detail?.message || detail?.error || (e as Error).message || 'Registration failed.';
  } finally {
    busy.value = false;
  }
}
</script>
