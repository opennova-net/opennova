<template>
  <div class="space-y-8">
    <header>
      <h1 class="text-3xl font-semibold text-ink">Account</h1>
      <p class="mt-1 text-sm text-ink-muted">Your NovaWorld account: the handle and password you log in with in-game.</p>
    </header>

    <section v-if="user" class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6" aria-labelledby="account-details">
      <h2 id="account-details" class="text-lg font-semibold text-ink">Details</h2>
      <dl class="grid gap-x-6 gap-y-2 text-sm sm:grid-cols-[auto_1fr]">
        <dt class="text-ink-muted">Username</dt>
        <dd class="break-words font-medium text-ink">{{ user.username }}</dd>
        <dt class="text-ink-muted">Role</dt>
        <dd class="text-ink">{{ user.role === 'admin' ? 'Admin: this account manages NovaWorld' : 'Player' }}</dd>
      </dl>
      <p v-if="isAdmin" class="text-sm">
        <RouterLink to="/admin" class="text-accent hover:text-ink-bright">Open the admin pages</RouterLink>
      </p>
    </section>

    <section class="space-y-3 rounded-panel border border-border bg-panel p-4 sm:p-6" aria-labelledby="account-session">
      <h2 id="account-session" class="text-lg font-semibold text-ink">Session</h2>
      <p class="text-sm text-ink-muted">Log out of this browser. Your hosted servers keep running.</p>
      <p v-if="error" role="alert" class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">{{ error }}</p>
      <button
        type="button"
        :disabled="signingOut"
        class="rounded-control border border-border-strong px-4 py-2 text-sm font-medium text-ink transition hover:border-accent disabled:opacity-50"
        @click="handleSignOut"
      >
        {{ signingOut ? 'Logging out…' : 'Log out' }}
      </button>
    </section>
  </div>
</template>

<script setup lang="ts">
// /me/account: who is logged in, and the log out.
import { ref } from 'vue';
import { RouterLink, useRouter } from 'vue-router';
import { useSession } from '../../composables/useSession';

const { user, isAdmin, signOut } = useSession();
const router = useRouter();
const signingOut = ref(false);
const error = ref('');

async function handleSignOut() {
  signingOut.value = true;
  error.value = '';
  const result = await signOut();
  signingOut.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    return;
  }
  await router.push('/');
}
</script>
