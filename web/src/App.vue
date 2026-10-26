<template>
  <div class="flex min-h-screen flex-col bg-surface text-ink">
    <a href="#content" class="sr-only focus:not-sr-only focus:fixed focus:left-4 focus:top-3 focus:z-30 focus:rounded-control focus:bg-accent focus:px-4 focus:py-2 focus:text-on-accent">
      Skip to content
    </a>
    <NavBar />
    <!-- The one <main>: its top padding clears the fixed header, so a page
         starts its own spacing from zero. -->
    <main id="content" ref="content" tabindex="-1" class="flex-1 pt-16 focus:outline-none" :class="admin ? '' : 'pb-16'">
      <RouterView />
    </main>
    <SiteFooter v-if="!admin" />
    <ConfirmDialog />
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue';
import { RouterView, useRoute } from 'vue-router';
import NavBar from './components/NavBar.vue';
import SiteFooter from './components/SiteFooter.vue';
import ConfirmDialog from './components/ConfirmDialog.vue';
import { settleConfirmation } from './composables/useConfirm';

const route = useRoute();
const admin = computed(() => route.path === '/admin' || route.path.startsWith('/admin/'));
const content = ref<HTMLElement | null>(null);
watch(() => route.fullPath, () => settleConfirmation(false));
watch(() => route.path, async () => {
  await nextTick();
  content.value?.focus({ preventScroll: true });
});
</script>
