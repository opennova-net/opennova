<template>
  <div class="space-y-6">
    <PageHeader title="Account">
      <template #actions>
        <RouterLink v-if="isAdmin" to="/admin" class="btn-secondary">Admin</RouterLink>
        <button type="button" class="btn-secondary" :disabled="signingOut" @click="handleSignOut">
          {{ signingOut ? 'Logging out…' : 'Log out' }}
        </button>
      </template>
    </PageHeader>

    <LiveNotices :error="error" />

    <dl v-if="user" class="card grid gap-x-6 gap-y-2 p-4 text-sm sm:grid-cols-[auto_1fr] sm:p-6">
      <dt class="text-ink-muted">Username</dt>
      <dd class="break-words font-medium text-ink">{{ user.username }}</dd>
      <dt class="text-ink-muted">Role</dt>
      <dd class="text-ink">{{ user.role === 'admin' ? 'Admin' : 'Player' }}</dd>
    </dl>
  </div>
</template>

<script setup lang="ts">
// /me/account: who is logged in, and the log out.
import { ref } from 'vue';
import { RouterLink, useRouter } from 'vue-router';
import LiveNotices from '../../components/ui/LiveNotices.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
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
