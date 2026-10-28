<template>
  <Workspace label="Your account" :items="items" :current="current" narrow>
    <RouterView />
    <template #footer>
      <div class="space-y-3 text-sm">
        <p v-if="user" class="account-user truncate font-semibold text-ink-bright">{{ user.username }}</p>
        <RouterLink to="/lobby" class="block text-ink-muted hover:text-accent">Find a game <span aria-hidden="true">↗</span></RouterLink>
        <RouterLink v-if="isAdmin" to="/admin" class="block text-ink-muted hover:text-accent">Admin</RouterLink>
        <button type="button" class="btn-ghost btn-sm -ml-2" :disabled="signingOut" @click="handleSignOut">{{ signingOut ? 'Logging out…' : 'Log out' }}</button>
        <p v-if="signOutError" role="alert" class="text-xs text-danger">{{ signOutError }}</p>
      </div>
    </template>
  </Workspace>
</template>

<script setup lang="ts">
// The player's own pages (/me/...): Overview, Servers, Projects and Billing
// down the side, who is logged in under them, in the frame the admin shares
// (components/ui/Workspace.vue). Its routes need a logged-in visitor.
import { computed, ref } from 'vue';
import { RouterLink, RouterView, useRoute, useRouter } from 'vue-router';
import Workspace from '../../components/ui/Workspace.vue';
import type { SideNavItem } from '../../components/ui/SideNav.vue';
import { useSession } from '../../composables/useSession';
import { ACCOUNT_SECTIONS, accountSectionOf } from '../../utils/accountNav';

const route = useRoute();
const router = useRouter();
const { user, isAdmin, signOut } = useSession();

const items: SideNavItem[] = ACCOUNT_SECTIONS.map((s) => ({ to: s.to, label: s.label }));
const current = computed(() => accountSectionOf(route.path)?.to ?? null);

const signingOut = ref(false);
const signOutError = ref('');

// Log out: leave the account's pages first (a dirty form may veto it), then
// end the session.
async function handleSignOut() {
  if (signingOut.value) return;
  signingOut.value = true;
  signOutError.value = '';
  try {
    if (await router.push('/')) return;
    const result = await signOut();
    if (!result.ok) signOutError.value = result.error.message;
  } finally {
    signingOut.value = false;
  }
}
</script>
