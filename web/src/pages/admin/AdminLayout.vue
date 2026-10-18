<template>
  <AdminGate>
    <div class="page-wide py-10">
      <!-- The nav's Log out ends a session and forgets the token; a token
           alone (no session) is forgotten here. -->
      <div v-if="hasAdminToken && !isAdmin" class="mb-6 flex items-center justify-end gap-4">
        <span class="text-xs text-ink-muted">Admin token</span>
        <button
          type="button"
          class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover"
          @click="clearAdminToken"
        >
          Forget token
        </button>
      </div>

      <div class="flex flex-col gap-8 md:flex-row">
        <nav aria-label="Admin" class="shrink-0 md:w-56">
          <ul
            ref="menu"
            class="flex gap-1 overflow-x-auto rounded-panel border border-border bg-panel p-2 [scrollbar-width:none] md:flex-col md:overflow-visible [&::-webkit-scrollbar]:hidden"
          >
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
                <!-- What needs an admin, counted on the page that lists it. -->
                <span
                  v-if="item.to === '/admin/overview' && needing > 0"
                  class="admin-attention-count ml-1.5 inline-flex min-w-[1.25rem] items-center justify-center rounded-full bg-danger px-1.5 text-xs font-semibold text-on-accent"
                  :title="`${needing} ${needing === 1 ? 'thing needs' : 'things need'} you`"
                >
                  {{ needing }}<span class="sr-only"> {{ needing === 1 ? 'thing needs' : 'things need' }} you</span>
                </span>
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
import { computed, nextTick, ref, watch } from 'vue';
import { useRoute } from 'vue-router';
import AdminGate from '../../components/AdminGate.vue';
import { attentionCount } from '../../api/attention';
import { clearAdminToken, hasAdminToken } from '../../api/authToken';
import { provideAttention } from '../../composables/useAttention';
import { useSession } from '../../composables/useSession';

interface AdminSection {
  to: string;
  label: string;
}

// /admin is a pure redirect to /admin/overview, so the route path is always
// one of the `to` values below or a page under one (a user's, a fleet
// server's, a job's).
const sections: AdminSection[] = [
  { to: '/admin/overview', label: 'Overview' },
  { to: '/admin/users', label: 'Users' },
  { to: '/admin/fleet', label: 'Fleet' },
  { to: '/admin/addresses', label: 'Static addresses' },
  { to: '/admin/data-sets', label: 'Game data' },
  { to: '/admin/usage', label: 'Server usage' },
  { to: '/admin/billing', label: 'Billing' },
  { to: '/admin/service', label: 'NovaWorld service' },
  { to: '/admin/sessions', label: 'NovaWorld sessions' },
  { to: '/admin/jobs', label: 'Background jobs' },
  { to: '/admin/infrastructure', label: 'Infrastructure' },
  { to: '/admin/cd-keys', label: 'CD keys' },
];

const route = useRoute();

// An explicit active check: RouterLink's default active class would light up
// every item whose path is a prefix of the route's.
function isActive(item: AdminSection): boolean {
  return route.path === item.to || route.path.startsWith(`${item.to}/`);
}

const { isAdmin } = useSession();

// What needs an admin (GET /api/admin/attention), read every 60 s while the
// gate is open and shared with the pages that list it (useAttention.ts); the
// nav counts the danger and warn findings and the firing alarms.
const attention = provideAttention(computed(() => isAdmin.value || hasAdminToken.value));
const needing = computed(() => (attention.attention.value ? attentionCount(attention.attention.value) : 0));

// On a phone the menu is one scrolling row: the current section is scrolled
// into it (the row, not the page).
const menu = ref<HTMLElement | null>(null);
function revealCurrent() {
  const row = menu.value;
  const current = row?.querySelector<HTMLElement>('[aria-current="page"]');
  if (!row || !current || row.scrollWidth <= row.clientWidth) return;
  const offset = current.getBoundingClientRect().left - row.getBoundingClientRect().left;
  row.scrollLeft += offset - (row.clientWidth - current.offsetWidth) / 2;
}
// The menu renders once the gate opens (after mount, the session read), then
// on every move between sections.
watch(
  [() => route.path, menu],
  () => void nextTick(revealCurrent),
  { immediate: true },
);
</script>
