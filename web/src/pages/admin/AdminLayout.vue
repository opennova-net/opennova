<template>
  <AdminGate>
    <div class="mx-auto max-w-6xl px-6 py-10">
      <div class="mb-8 flex items-center justify-between">
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
        <aside class="shrink-0 md:w-56">
          <nav class="flex gap-1 overflow-x-auto rounded-panel border border-border bg-panel p-2 md:flex-col md:overflow-visible">
            <RouterLink
              v-for="item in sections"
              :key="item.to"
              :to="item.to"
              class="whitespace-nowrap rounded-control px-3 py-2 text-sm font-medium transition"
              :class="isActive(item)
                ? 'bg-raised text-accent'
                : 'text-ink-muted hover:bg-hover hover:text-ink'"
            >
              {{ item.label }}
            </RouterLink>
          </nav>
        </aside>

        <main class="min-w-0 flex-1">
          <RouterView />
        </main>
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

// /admin is a pure redirect to /admin/users, so the route path is always one
// of the `to` values below.
const sections: AdminSection[] = [
  { to: '/admin/users', label: 'Users' },
  { to: '/admin/server', label: 'Server Status' },
  { to: '/admin/connections', label: 'Connections' },
];

const route = useRoute();

// Explicit active check — RouterLink's default active class would light up the
// first item on every child path since '/admin' is a prefix of them all.
function isActive(item: AdminSection): boolean {
  return route.path === item.to;
}

const { user, isAdmin, signOut: endSession } = useSession();
const signingOut = ref(false);

// Which credentials the admin requests carry. With both, the server tries the
// token first and falls back to the session when it does not match.
const credentialLabel = computed(() => {
  const session = isAdmin.value ? `Logged in as ${user.value?.username}` : '';
  if (!hasAdminToken.value) return session;
  return session ? `${session} · admin token stored` : 'Admin token';
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
