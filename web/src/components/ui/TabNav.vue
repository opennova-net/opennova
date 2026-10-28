<template>
  <nav :aria-label="label" class="tab-nav border-b border-border">
    <ul class="-mb-px flex gap-1 overflow-x-auto">
      <li v-for="tab in tabs" :key="tab.label">
        <RouterLink
          :to="tab.to"
          class="tab-link"
          :class="isActive(tab) ? 'border-accent text-ink-bright' : ''"
          :aria-current="isActive(tab) ? 'page' : undefined"
        >
          {{ tab.label }}
          <span v-if="tab.count" class="badge badge-info">{{ tab.count }}</span>
        </RouterLink>
      </li>
    </ul>
  </nav>
</template>

<script lang="ts">
import type { RouteLocationRaw } from 'vue-router';

export interface Tab {
  label: string;
  to: RouteLocationRaw;
  // A number worth a look (awaiting approvals): a badge beside the label.
  count?: number;
  // Current on its path whatever its ?tab= (a page with views of its own).
  pathOnly?: boolean;
}
</script>

<script setup lang="ts">
// A section's tabs, one look everywhere: each a route (a child path, or the
// same path with ?tab=), the current one underlined in the accent.
import { RouterLink, useRoute, useRouter } from 'vue-router';

defineProps<{ tabs: Tab[]; label: string }>();

const route = useRoute();
const router = useRouter();

// The tab whose path is this page's; a ?tab= tab also matches its query.
function isActive(tab: Tab): boolean {
  const target = router.resolve(tab.to);
  if (target.path !== route.path) return false;
  if (tab.pathOnly) return true;
  const wanted = target.query.tab;
  return (route.query.tab ?? undefined) === (wanted ?? undefined);
}
</script>
