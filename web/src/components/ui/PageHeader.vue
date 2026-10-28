<template>
  <div class="page-header space-y-3">
    <nav v-if="crumbs.length" aria-label="Breadcrumb" class="page-crumbs text-sm">
      <ol class="flex flex-wrap items-center gap-x-2 gap-y-1 text-ink-muted">
        <li v-for="(crumb, i) in crumbs" :key="crumb.label" class="flex items-center gap-2">
          <span v-if="i > 0" aria-hidden="true">/</span>
          <RouterLink v-if="crumb.to" :to="crumb.to" class="entity-link">{{ crumb.label }}</RouterLink>
          <span v-else>{{ crumb.label }}</span>
        </li>
      </ol>
    </nav>
    <header class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
      <div class="min-w-0 space-y-1.5">
        <h1 class="page-heading break-words">
          <slot name="title">{{ title }}</slot>
        </h1>
        <div v-if="$slots.meta" class="page-meta flex flex-wrap items-center gap-x-3 gap-y-1 text-sm text-ink-muted">
          <slot name="meta" />
        </div>
      </div>
      <div v-if="refreshable || $slots.actions" class="flex shrink-0 flex-wrap items-center gap-2">
        <p v-if="loadedAt" class="text-xs text-ink-muted">Updated <RelativeTime :value="loadedAt" :now="now ?? loadedAt" /></p>
        <slot name="actions" />
        <button v-if="refreshable" type="button" class="btn-secondary" :disabled="loading" @click="emit('refresh')">
          {{ loading ? 'Refreshing…' : 'Refresh' }}
        </button>
      </div>
    </header>
    <TabNav v-if="tabs.length" :tabs="tabs" :label="`${title} pages`" />
  </div>
</template>

<script setup lang="ts">
// Every page's head, admin, account and public alike: where it sits (crumbs,
// for a page under a section), its title, one line of facts about it (the
// `meta` slot: a state badge, its game, its owner), its actions on the right
// with when it was last read and its Refresh, and its section's tabs. No page
// carries a description: the title and the data say what it is.
import { RouterLink } from 'vue-router';
import RelativeTime from '../RelativeTime.vue';
import TabNav, { type Tab } from './TabNav.vue';
import type { Crumb } from '../../utils/adminNav';

withDefaults(
  defineProps<{
    title: string;
    crumbs?: Crumb[];
    tabs?: Tab[];
    // When the page last read what it shows (ms); null before its first answer.
    loadedAt?: number | null;
    // The page's clock (ms), for "Updated 3 min ago".
    now?: number;
    loading?: boolean;
    refreshable?: boolean;
  }>(),
  { crumbs: () => [], tabs: () => [], loadedAt: null, now: undefined, loading: false, refreshable: false },
);

const emit = defineEmits<{ refresh: [] }>();
</script>
