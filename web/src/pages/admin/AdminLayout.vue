<template>
  <AdminGate>
    <div class="mx-auto max-w-6xl px-4 py-10 sm:px-6">
      <div class="mb-8 flex items-center justify-between gap-4">
        <div class="flex items-center gap-2 text-xs uppercase tracking-[0.3em] text-ink-muted">
          <span>Admin</span>
        </div>
        <div class="flex items-center gap-4">
          <span class="hidden text-xs text-ink-muted sm:inline">{{ credentialLabel }}</span>
          <button
            type="button"
            class="rounded-control border border-danger/40 bg-danger/10 px-4 py-2 text-sm font-medium text-danger transition hover:bg-danger/20 disabled:opacity-50"
            :disabled="signingOut"
            @click="signOut"
          >
            Sign out
          </button>
        </div>
      </div>

      <div class="flex flex-col gap-8 md:flex-row">
        <nav aria-label="Admin" class="shrink-0 md:w-56">
          <ul class="flex gap-1 overflow-x-auto rounded-panel border border-border bg-panel p-2 md:flex-col md:overflow-visible">
            <li v-for="item in sections" :key="item.to">
              <RouterLink
                :to="item.to"
                class="block whitespace-nowrap rounded-control px-3 py-2 text-sm font-medium transition"
                :class="isActive(item)
                  ? 'bg-raised text-accent'
                  : 'text-ink-muted hover:bg-hover hover:text-ink'"
                :aria-current="isActive(item) ? 'page' : undefined"
              >
                {{ item.label }}
              </RouterLink>
            </li>
          </ul>
        </nav>

        <div class="min-w-0 flex-1">
          <RouterView />
        </div>
      </div>
    </div>
  </AdminGate>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue';
import { useRoute } from 'vue-router';
import AdminGate from '../../components/AdminGate.vue';
import { clearAdminToken, hasAdminToken } from '../../api/authToken';
import { useSession } from '../../composables/useSession';

interface AdminSection {
  to: string;
  label: string;
}

// /admin is a pure redirect to /admin/overview, so the route path is always
// one of the `to` values below or a page under one (a user's, a fleet
// server's).
const sections: AdminSection[] = [
  { to: '/admin/overview', label: 'Overview' },
  { to: '/admin/users', label: 'Users' },
  { to: '/admin/fleet', label: 'Fleet' },
  { to: '/admin/billing', label: 'Billing' },
  { to: '/admin/service', label: 'NovaWorld service' },
  { to: '/admin/sessions', label: 'NovaWorld sessions' },
  { to: '/admin/cd-keys', label: 'CD keys' },
];

const route = useRoute();

// An explicit active check: RouterLink's default active class would light up
// every item whose path is a prefix of the route's.
function isActive(item: AdminSection): boolean {
  return route.path === item.to || route.path.startsWith(`${item.to}/`);
}

const { user, isAdmin, signOut: endSession } = useSession();
const signingOut = ref(false);

// Which credentials the admin requests carry. With both, the server tries the
// token first and falls back to the session when it does not match, so
// while the session is an admin's a wrong token is never refused: the label
// does not vouch for it.
const credentialLabel = computed(() => {
  const session = isAdmin.value ? `Logged in as ${user.value?.username}` : '';
  if (!hasAdminToken.value) return session;
  return session ? `${session} · stored admin token (unverified)` : 'Admin token';
});

// Drops every admin credential: the stored token and an admin session (a
// player's session is not an admin credential and stays). The gate then asks
// again.
async function signOut() {
  signingOut.value = true;
  clearAdminToken();
  if (isAdmin.value) {
    const result = await endSession();
    if (!result.ok) window.alert(result.error.message);
  }
  signingOut.value = false;
}
</script>
