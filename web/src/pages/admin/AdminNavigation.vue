<template>
  <nav aria-label="Admin">
    <ul class="space-y-1">
      <li v-for="item in ADMIN_SECTIONS" :key="item.to">
        <RouterLink :to="item.to" :class="linkClass(item)" :aria-current="isActive(item) ? 'page' : undefined" @click="emit('navigate')">
          {{ item.label }}
          <template v-if="item.to === '/admin'">
            <span v-if="unread" class="admin-attention-unknown badge" title="What needs you could not be read"><span aria-hidden="true">?</span><span class="sr-only">What needs you could not be read</span></span>
            <span v-else-if="needing > 0" class="admin-attention-count badge badge-bad">{{ needing }}<span class="sr-only">{{ needing === 1 ? ' thing needs you' : ' things need you' }}</span></span>
          </template>
          <span v-else-if="item.to === '/admin/products' && awaiting > 0" class="admin-awaiting-count badge badge-warn">{{ awaiting }}<span class="sr-only"> awaiting approval</span></span>
        </RouterLink>
      </li>
    </ul>
  </nav>
</template>

<script setup lang="ts">
// The admin's sidebar: its six sections, the current one marked; the
// Overview carries how many things need an admin, Products how many releases
// wait for one.
import { RouterLink, useRoute } from 'vue-router';
import { ADMIN_SECTIONS, sectionOf, type AdminSection } from '../../utils/adminNav';

withDefaults(defineProps<{ unread: boolean; needing: number; awaiting?: number }>(), { awaiting: 0 });
const emit = defineEmits<{ navigate: [] }>();
const route = useRoute();
function isActive(item: AdminSection) {
  return sectionOf(route.path)?.to === item.to;
}
function linkClass(item: AdminSection) {
  return ['workspace-nav-link', isActive(item) ? 'bg-accent/10 text-accent ring-1 ring-inset ring-accent/20' : ''];
}
</script>
