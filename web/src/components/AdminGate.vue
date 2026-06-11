<template>
  <div v-if="!hasToken" class="mx-auto max-w-md px-6 py-10">
    <div class="space-y-6 rounded-xl border border-white/10 bg-white/5 p-8">
      <div>
        <h1 class="text-2xl font-semibold text-white">Admin login</h1>
        <p class="mt-2 text-sm text-slate-300">
          Enter the bearer token configured in <code class="font-mono text-xs">ADMIN_API_TOKEN</code>
          on the standalone server.
        </p>
      </div>
      <form class="space-y-4" @submit.prevent="submit">
        <label class="block">
          <span class="text-xs font-medium uppercase tracking-wider text-slate-400">Admin token</span>
          <input
            v-model="tokenInput"
            type="password"
            class="mt-1 w-full rounded-md border border-white/10 bg-slate-900/60 px-3 py-2 text-sm text-white focus:border-emerald-400/60 focus:outline-none"
            autofocus
            required
          />
        </label>
        <button
          type="submit"
          class="w-full rounded-md bg-emerald-500 px-4 py-2 text-sm font-medium text-white transition hover:bg-emerald-400"
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
