<template>
  <AdminGate>
    <div class="admin-workspace min-h-[calc(100vh-4rem)] lg:grid lg:grid-cols-[240px_minmax(0,1fr)]">
      <div class="flex items-center justify-between border-b border-border bg-panel px-5 py-3 lg:hidden">
        <span class="text-sm font-semibold text-ink">Administration</span>
        <button ref="menuButton" type="button" class="rounded-control border border-border px-3 py-2 text-sm text-ink" aria-controls="admin-drawer" :aria-expanded="drawerOpen" @click="openDrawer">Browse admin</button>
      </div>
      <aside class="sticky top-16 hidden h-[calc(100vh-4rem)] overflow-y-auto border-r border-border bg-panel p-4 lg:block">
        <div class="mb-7 px-3 pt-2">
          <p class="text-xs font-semibold uppercase tracking-[0.18em] text-accent">Workspace</p>
          <p class="mt-1 text-sm font-semibold text-ink">Administration</p>
        </div>
        <AdminNavigation id-base="admin-nav" :unread="unread" :needing="needing" />
        <div v-if="hasAdminToken && !isAdmin" class="mt-8 border-t border-border px-3 pt-4">
          <p class="mb-2 text-xs text-ink-muted">Using an admin token</p>
          <button type="button" class="text-sm text-accent hover:text-ink" @click="clearAdminToken">Forget token</button>
        </div>
      </aside>
      <dialog id="admin-drawer" ref="drawer" aria-label="Admin navigation" class="admin-drawer m-0 h-dvh max-h-none w-72 max-w-[85vw] border-r border-border bg-panel p-5 text-ink backdrop:bg-black/60" @close="onClosed" @click="onBackdrop">
        <div class="mb-6 flex items-center justify-between">
          <h2 class="font-semibold">Administration</h2>
          <button type="button" class="rounded-control border border-border px-3 py-2 text-sm" @click="closeDrawer">Close</button>
        </div>
        <AdminNavigation v-if="drawerOpen" id-base="admin-drawer-nav" :unread="unread" :needing="needing" @navigate="closeDrawer" />
        <button v-if="drawerOpen && hasAdminToken && !isAdmin" type="button" class="mt-6 px-3 text-sm text-accent" @click="clearAdminToken">Forget token</button>
      </dialog>
      <div class="min-w-0 px-5 py-7 sm:px-8 lg:px-10 lg:py-9 2xl:px-14"><RouterView /></div>
    </div>
  </AdminGate>
</template>

<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue';
import { useRoute } from 'vue-router';
import AdminGate from '../../components/AdminGate.vue';
import AdminNavigation from './AdminNavigation.vue';
import { attentionCount } from '../../api/attention';
import { clearAdminToken, hasAdminToken } from '../../api/authToken';
import { provideAttention } from '../../composables/useAttention';
import { useSession } from '../../composables/useSession';
const route = useRoute();
const { isAdmin } = useSession();
const drawer = ref<HTMLDialogElement | null>(null);
const menuButton = ref<HTMLButtonElement | null>(null);
const drawerOpen = ref(false);
let previousOverflow = '';
async function openDrawer() {
  if (drawerOpen.value) return;
  previousOverflow = document.body.style.overflow;
  document.body.style.overflow = 'hidden';
  drawerOpen.value = true;
  await nextTick();
  drawer.value?.showModal();
  drawer.value?.querySelector<HTMLElement>('[aria-current="page"]')?.focus();
}
function closeDrawer() { drawer.value?.close(); }
function onClosed() {
  drawerOpen.value = false;
  document.body.style.overflow = previousOverflow;
  menuButton.value?.focus();
}
function onBackdrop(event: MouseEvent) {
  const element = drawer.value;
  if (!element || event.target !== element) return;
  const bounds = element.getBoundingClientRect();
  if (event.clientX < bounds.left || event.clientX > bounds.right || event.clientY < bounds.top || event.clientY > bounds.bottom) closeDrawer();
}
function onResize() { if (drawerOpen.value && window.innerWidth >= 1024) closeDrawer(); }
watch(() => route.fullPath, () => { if (drawerOpen.value) closeDrawer(); });
onMounted(() => window.addEventListener('resize', onResize));
onUnmounted(() => {
  window.removeEventListener('resize', onResize);
  if (drawerOpen.value) document.body.style.overflow = previousOverflow;
});
const attention = provideAttention(computed(() => isAdmin.value || hasAdminToken.value));
const unread = computed(() => attention.error.value !== '');
const needing = computed(() => attention.attention.value ? attentionCount(attention.attention.value) : 0);
</script>
