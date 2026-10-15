<template>
  <div class="mx-auto max-w-6xl px-4 py-10 sm:px-6">
    <div class="mb-8 flex items-center justify-between gap-4">
      <p class="text-xs uppercase tracking-[0.3em] text-ink-muted">Your account</p>
      <p v-if="user" class="truncate text-xs text-ink-muted">
        Logged in as <span class="font-medium text-ink">{{ user.username }}</span>
      </p>
    </div>

    <div class="flex flex-col gap-8 md:flex-row">
      <nav aria-label="Your account" class="shrink-0 md:w-56">
        <ul class="flex gap-1 overflow-x-auto rounded-panel border border-border bg-panel p-2 md:flex-col md:overflow-visible">
          <li v-for="item in sections" :key="item.to">
            <RouterLink
              :to="item.to"
              class="block whitespace-nowrap rounded-control px-3 py-2 text-sm font-medium transition"
              :class="isActive(item) ? 'bg-raised text-accent' : 'text-ink-muted hover:bg-hover hover:text-ink'"
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
</template>

<script setup lang="ts">
// The account area (/me/...): a side nav over its pages, modelled on the admin
// layout. Its routes need a logged-in visitor (the router's requiresUser).
import { RouterLink, RouterView, useRoute } from 'vue-router';
import { useSession } from '../../composables/useSession';

interface AccountSection {
  to: string;
  label: string;
}

const sections: AccountSection[] = [
  { to: '/me/servers', label: 'Servers' },
  { to: '/me/account', label: 'Account' },
];

const route = useRoute();
const { user } = useSession();

// A section is active on its own path and every path under it (a server's
// page is under Servers).
function isActive(item: AccountSection): boolean {
  return route.path === item.to || route.path.startsWith(`${item.to}/`);
}
</script>
