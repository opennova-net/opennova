<template>
  <section class="px-6 pt-24 pb-16">
    <div class="mx-auto max-w-3xl rounded-panel border border-border bg-panel p-8">
      <h1 class="text-3xl font-semibold text-ink">Register</h1>
      <p class="mt-2 text-sm text-ink-muted">
        Pick a handle and a password. That is what you log in with in-game.
      </p>
      <form class="mt-8 grid gap-6" @submit.prevent="handleSubmit">
        <label class="grid gap-2 text-sm text-ink">
          Game Handle
          <input v-model="form.username" type="text" required minlength="2" maxlength="32"
                 class="rounded-control border border-border bg-surface px-4 py-3 focus:border-accent focus:outline-none" />
        </label>
        <label class="grid gap-2 text-sm text-ink">
          Display Name (NW Handle)
          <input v-model="form.nwhandle" type="text" maxlength="32"
                 placeholder="defaults to handle"
                 class="rounded-control border border-border bg-surface px-4 py-3 focus:border-accent focus:outline-none" />
        </label>
        <label class="grid gap-2 text-sm text-ink">
          Password
          <input v-model="form.password" type="password" required minlength="3" maxlength="72"
                 class="rounded-control border border-border bg-surface px-4 py-3 focus:border-accent focus:outline-none" />
        </label>
        <button type="submit" :disabled="busy"
                class="rounded-control bg-accent px-6 py-3 font-semibold text-on-accent hover:bg-accent/90 disabled:opacity-50">
          {{ busy ? 'Creating account…' : 'Register Account' }}
        </button>
        <p v-if="success" class="rounded-panel border border-online/40 bg-online/10 p-3 text-sm text-online">
          Account created! Username <code class="font-mono">{{ success.username }}</code>, PCID
          <code class="font-mono">{{ success.pcid }}</code>. You can now log in via Joint Operations.
        </p>
        <p v-if="error" class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">
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
