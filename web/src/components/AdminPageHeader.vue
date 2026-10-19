<template>
  <div class="admin-page-header space-y-2">
    <AdminCrumbs v-if="crumbs" :trail="trail" />
    <header class="flex flex-col gap-4 sm:flex-row sm:items-start sm:justify-between">
      <div class="min-w-0 space-y-1">
        <h1 class="break-words text-3xl font-semibold text-ink">{{ title }}</h1>
        <p v-if="description || $slots.description" class="text-sm text-ink-muted">
          <slot name="description">{{ description }}</slot>
        </p>
        <slot />
      </div>
      <div v-if="refreshable || $slots.actions" class="flex shrink-0 flex-wrap items-center gap-3">
        <slot name="actions" />
        <p v-if="loadedAt" class="text-xs text-ink-muted">Updated <RelativeTime :value="loadedAt" :now="now ?? loadedAt" /></p>
        <button v-if="refreshable" type="button" :class="secondaryButton" :disabled="loading" @click="emit('refresh')">
          {{ loading ? 'Refreshing…' : 'Refresh' }}
        </button>
      </div>
    </header>
  </div>
</template>

<script setup lang="ts">
// Every admin page's head, the same on each: where it sits (AdminCrumbs: its
// area, and its section's page from a page under one), its name, what it holds
// in a line (or the `description` slot, with links), anything more in the
// default slot; on the right its own actions, when it was last read and its
// Refresh, which says while it reads.
import AdminCrumbs from './AdminCrumbs.vue';
import RelativeTime from './RelativeTime.vue';
import type { Crumb } from '../utils/adminNav';

withDefaults(
  defineProps<{
    title: string;
    description?: string;
    // When the page last read what it shows (ms); null before its first answer.
    loadedAt?: number | null;
    // The page's clock (ms), for "Updated 3 min ago".
    now?: number;
    loading?: boolean;
    refreshable?: boolean;
    crumbs?: boolean;
    trail?: Crumb[];
  }>(),
  { description: '', loadedAt: null, now: undefined, loading: false, refreshable: true, crumbs: true, trail: () => [] },
);

const emit = defineEmits<{ refresh: [] }>();

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';
</script>
