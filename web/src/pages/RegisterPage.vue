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
          <input v-model="form.username" type="text" required minlength="2" maxlength="30"
                 class="rounded-control border border-border bg-surface px-4 py-3 focus:border-accent focus:outline-none" />
        </label>
        <label class="grid gap-2 text-sm text-ink">
          Display Name (NW Handle)
          <input v-model="form.nwhandle" type="text" maxlength="30"
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
        <div
          v-if="success"
          role="status"
          class="flex flex-wrap items-center justify-between gap-3 rounded-panel border border-online/40 bg-online/10 p-3 text-sm text-online"
        >
          <p>
            Account created! Username <code class="font-mono">{{ success.username }}</code>, PCID
            <code class="font-mono">{{ success.pcid }}</code>. You can now log in via Joint Operations,
            or log in to this site.
          </p>
          <RouterLink
            ref="loginOffer"
            :to="loginLink(success.username)"
            class="rounded-control bg-accent px-4 py-2 font-semibold text-on-accent hover:bg-accent/90"
          >
            Log in
          </RouterLink>
        </div>
        <p v-if="error" role="alert" class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">
          {{ error }}
        </p>
      </form>
      <p class="mt-6 text-sm text-ink-muted">
        Already have an account?
        <RouterLink :to="loginLink()" class="text-accent hover:text-ink-bright">Log in</RouterLink>
      </p>
    </div>
  </section>
</template>

<script setup lang="ts">
import { nextTick, reactive, ref } from 'vue';
import { RouterLink, useRoute, type RouteLocationRaw } from 'vue-router';
import { registerUser } from '../api/users';
import { safeRedirect } from '../router/guards';
import type { User } from '../types/users';

const route = useRoute();

// The login page, carrying on the ?redirect= this page was opened with and,
// after a registration, the new username.
function loginLink(username?: string): RouteLocationRaw {
  const query: Record<string, string> = {};
  const redirect = safeRedirect(route.query.redirect);
  if (redirect !== '/') query.redirect = redirect;
  if (username) query.username = username;
  return { path: '/login', query };
}

const loginOffer = ref<{ $el: HTMLElement } | null>(null);

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
    // The next step is the login: put the keyboard on it.
    await nextTick();
    loginOffer.value?.$el.focus();
  } catch (e) {
    const detail = (e as { response?: { data?: { error?: string; message?: string } } })
      ?.response?.data;
    error.value = detail?.message || detail?.error || (e as Error).message || 'Registration failed.';
  } finally {
    busy.value = false;
  }
}
</script>
