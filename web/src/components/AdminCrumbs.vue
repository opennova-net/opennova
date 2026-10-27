<template>
  <nav v-if="section && !onSection" aria-label="Breadcrumb" class="admin-crumbs text-sm">
    <ol class="flex flex-wrap items-center gap-x-2 gap-y-1 text-ink-muted">
      <li><RouterLink :to="section.to" class="entity-link">{{ section.label }}</RouterLink></li>
      <li v-for="crumb in trail" :key="crumb.label">
        <span aria-hidden="true" class="mr-2">/</span>
        <RouterLink v-if="crumb.to" :to="crumb.to" class="entity-link">{{ crumb.label }}</RouterLink>
        <span v-else>{{ crumb.label }}</span>
      </li>
    </ol>
  </nav>
</template>

<script setup lang="ts">
// Where an admin page sits (utils/adminNav.ts): its section as a link when
// this is a page under it, then any further steps the page names.
import { computed } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import { sectionOf, type Crumb } from '../utils/adminNav';

withDefaults(defineProps<{ trail?: Crumb[] }>(), { trail: () => [] });

// No router (a page mounted alone in a test): no breadcrumb.
const route = useRoute() as ReturnType<typeof useRoute> | undefined;
const path = computed(() => route?.path ?? '');
const section = computed(() => sectionOf(path.value));
const onSection = computed(() => section.value !== null && path.value === section.value.to);
</script>
