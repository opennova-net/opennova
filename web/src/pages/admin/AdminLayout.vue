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
          <!-- One scrolling row on a phone (the areas side by side), a column
               with each area's name above its pages from md up. -->
          <div
            ref="menu"
            class="flex items-stretch gap-1 overflow-x-auto rounded-panel border border-border bg-panel p-2 [scrollbar-width:none] md:flex-col md:gap-4 md:overflow-visible [&::-webkit-scrollbar]:hidden"
          >
            <ul class="flex gap-1 md:flex-col">
              <li>
                <RouterLink
                  :to="OVERVIEW.to"
                  class="flex items-center whitespace-nowrap rounded-control px-3 py-2 text-sm font-medium transition"
                  :class="linkClass(OVERVIEW)"
                  :aria-current="isActive(OVERVIEW) ? 'page' : undefined"
                >
                  {{ OVERVIEW.label }}
                  <!-- What needs an admin, counted on the page that lists it; a
                       mark of its own while the read fails, never a stale count. -->
                  <span
                    v-if="unread"
                    class="admin-attention-unknown ml-1.5 inline-flex min-w-[1.25rem] items-center justify-center rounded-full border border-border bg-raised px-1.5 text-xs font-semibold text-ink-muted"
                    title="What needs you could not be read"
                  >
                    <span aria-hidden="true">?</span><span class="sr-only">What needs you could not be read</span>
                  </span>
                  <span
                    v-else-if="needing > 0"
                    class="admin-attention-count ml-1.5 inline-flex min-w-[1.25rem] items-center justify-center rounded-full bg-danger px-1.5 text-xs font-semibold text-on-accent"
                    :title="`${needing} ${needing === 1 ? 'thing needs' : 'things need'} you`"
                  >
                    {{ needing }}<span class="sr-only">{{ ` ${needing === 1 ? 'thing needs' : 'things need'} you` }}</span>
                  </span>
                </RouterLink>
              </li>
            </ul>
            <div
              v-for="area in ADMIN_AREAS"
              :key="area.id"
              class="admin-nav-area flex shrink-0 items-center gap-1 border-l border-border pl-1 md:block md:border-l-0 md:pl-0"
              role="group"
              :aria-labelledby="`admin-nav-${area.id}`"
            >
              <p
                :id="`admin-nav-${area.id}`"
                class="sr-only md:not-sr-only md:px-3 md:pb-1 md:text-xs md:font-semibold md:uppercase md:tracking-wider md:text-ink-muted"
              >
                {{ area.label }}
              </p>
              <ul class="flex gap-1 md:flex-col">
                <li v-for="item in area.sections" :key="item.to">
                  <RouterLink
                    :to="item.to"
                    class="block whitespace-nowrap rounded-control px-3 py-2 text-sm font-medium transition"
                    :class="linkClass(item)"
                    :aria-current="isActive(item) ? 'page' : undefined"
                  >
                    {{ item.label }}
                  </RouterLink>
                </li>
              </ul>
            </div>
          </div>
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
import { ADMIN_AREAS, OVERVIEW, isUnder, type AdminSection } from '../../utils/adminNav';

// /admin is a pure redirect to /admin/overview, so the route path is always
// the Overview's or a section's page, or a page under one (a user's, a fleet
// server's, a job's): utils/adminNav.ts places it.
const route = useRoute();

// An explicit active check: RouterLink's default active class would light up
// every item whose path is a prefix of the route's.
function isActive(item: AdminSection): boolean {
  return isUnder(route.path, item);
}

function linkClass(item: AdminSection): string {
  return isActive(item) ? 'bg-raised text-accent' : 'text-ink-muted hover:bg-hover hover:text-ink';
}

const { isAdmin } = useSession();

// What needs an admin (GET /api/admin/attention), read every 60 s while the
// gate is open and shared with the pages that list it (useAttention.ts); the
// nav counts the danger and warn findings and the firing alarms, or shows a
// question mark while the read fails (the last count may be out of date).
const attention = provideAttention(computed(() => isAdmin.value || hasAdminToken.value));
const unread = computed(() => attention.error.value !== '');
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
