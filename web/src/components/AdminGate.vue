<template>
  <div v-if="!ready" class="mx-auto max-w-md px-6 py-10 text-center text-sm text-ink-muted" role="status">
    Checking your session…
  </div>
  <slot v-else-if="isAdmin || hasAdminToken" />
  <div v-else class="mx-auto max-w-md px-6 py-10">
    <div class="space-y-6 rounded-panel border border-border bg-raised p-8">
      <div>
        <h1 class="text-2xl font-semibold text-ink">Admin login</h1>
        <p v-if="user" class="mt-2 text-sm text-ink-muted">
          You are logged in as <span class="font-semibold text-ink">{{ user.username }}</span>, which is
          not an admin account.
        </p>
        <p v-else class="mt-2 text-sm text-ink-muted">
          <RouterLink :to="loginLink" class="text-accent hover:text-ink-bright">Log in</RouterLink>
          with an admin account, or use the server's admin token.
        </p>
        <p class="mt-2 text-sm text-ink-muted">
          Enter the bearer token configured in <code class="font-mono text-xs">ADMIN_API_TOKEN</code>
          on the standalone server.
        </p>
      </div>
      <form class="space-y-4" @submit.prevent="submit">
        <label class="block">
          <span class="text-xs font-medium uppercase tracking-wider text-ink-muted">Admin token</span>
          <input
            v-model="tokenInput"
            type="password"
            autocomplete="off"
            class="mt-1 w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
            autofocus
            required
          />
        </label>
        <button
          type="submit"
          class="w-full rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90"
        >
          Sign in
        </button>
      </form>
    </div>
  </div>
</template>

<script setup lang="ts">
// The /admin pages' gate. An admin-role session passes straight through (the
// server reads the role on every /api/admin request). Anyone else is asked
// for the server's ADMIN_API_TOKEN, the maintainer's machine credential,
// kept in localStorage (api/authToken.ts). When the server refuses a
// request, the adminClient drops a refused token and re-reads the session,
// and the gate closes again on its own.
import { computed, ref } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import { hasAdminToken, setAdminToken } from '../api/authToken';
import { useSession } from '../composables/useSession';

const { user, ready, isAdmin } = useSession();
const route = useRoute();
const tokenInput = ref('');

const loginLink = computed(() => ({ path: '/login', query: { redirect: route.fullPath } }));

function submit() {
  setAdminToken(tokenInput.value.trim());
  tokenInput.value = '';
}
</script>
