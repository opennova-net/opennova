<template>
  <nav aria-label="Admin" class="space-y-6">
    <RouterLink :to="OVERVIEW.to" :class="linkClass(OVERVIEW)" :aria-current="isActive(OVERVIEW) ? 'page' : undefined" @click="emit('navigate')">
      {{ OVERVIEW.label }}
      <span v-if="unread" class="admin-attention-unknown rounded-full border border-border px-2 text-xs" title="What needs you could not be read"><span aria-hidden="true">?</span><span class="sr-only">What needs you could not be read</span></span>
      <span v-else-if="needing > 0" class="admin-attention-count rounded-full bg-danger px-2 text-xs font-semibold text-on-accent">{{ needing }}<span class="sr-only">{{ needing === 1 ? ' thing needs you' : ' things need you' }}</span></span>
    </RouterLink>
    <div v-for="area in ADMIN_AREAS" :key="area.id" class="admin-nav-area" role="group" :aria-labelledby="idBase + '-' + area.id">
      <p :id="idBase + '-' + area.id" class="mb-2 px-3 text-[11px] font-semibold uppercase tracking-[0.16em] text-ink-muted">{{ area.label }}</p>
      <ul class="space-y-1">
        <li v-for="item in area.sections" :key="item.to">
          <RouterLink :to="item.to" :class="linkClass(item)" :aria-current="isActive(item) ? 'page' : undefined" @click="emit('navigate')">{{ item.label }}</RouterLink>
        </li>
      </ul>
    </div>
  </nav>
</template>

<script setup lang="ts">
import { useRoute } from 'vue-router';
import { ADMIN_AREAS, OVERVIEW, isUnder, type AdminSection } from '../../utils/adminNav';
defineProps<{ idBase: string; unread: boolean; needing: number }>();
const emit = defineEmits<{ navigate: [] }>();
const route = useRoute();
function isActive(item: AdminSection) { return isUnder(route.path, item); }
function linkClass(item: AdminSection) {
  const base = 'flex min-h-11 items-center justify-between rounded-control px-3 py-2 text-sm font-medium transition ';
  return base + (isActive(item) ? 'bg-accent/10 text-accent ring-1 ring-inset ring-accent/20' : 'text-ink-muted hover:bg-hover hover:text-ink');
}
</script>
