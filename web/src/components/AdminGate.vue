<template>
  <div v-if="!hasToken" class="mx-auto max-w-md px-6 py-10">
    <div class="space-y-6 rounded-panel border border-border bg-raised p-8">
      <div>
        <h1 class="text-2xl font-semibold text-ink">Admin login</h1>
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
  <slot v-else />
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { getAdminToken, setAdminToken } from '../api/authToken';

const hasToken = ref(Boolean(getAdminToken()));
const tokenInput = ref('');

function submit() {
  setAdminToken(tokenInput.value.trim());
  hasToken.value = Boolean(getAdminToken());
  tokenInput.value = '';
}
</script>
