<template>
  <div class="workspace min-h-[calc(100vh-4rem)] lg:grid lg:grid-cols-[240px_minmax(0,1fr)]">
    <div class="flex items-center justify-between border-b border-border bg-panel px-4 py-3 lg:hidden">
      <span class="text-sm font-semibold text-ink">{{ label }}</span>
      <button ref="menuButton" type="button" class="btn-secondary btn-sm" :aria-controls="drawerId" :aria-expanded="drawerOpen" @click="openDrawer">Menu</button>
    </div>
    <aside class="sticky top-16 hidden h-[calc(100vh-4rem)] flex-col overflow-y-auto border-r border-border bg-panel p-4 lg:flex">
      <SideNav :label="label" :items="items" :current="current" />
      <div v-if="$slots.footer" class="workspace-footer mt-auto border-t border-border pt-4"><slot name="footer" /></div>
    </aside>
    <dialog :id="drawerId" ref="drawer" :aria-label="label" class="workspace-drawer m-0 h-dvh max-h-none w-72 max-w-[85vw] border-r border-border bg-panel p-4 text-ink backdrop:bg-black/60" @close="onClosed" @click="onBackdrop">
      <div class="mb-4 flex items-center justify-between">
        <h2 class="font-semibold">{{ label }}</h2>
        <button type="button" class="btn-secondary btn-sm" @click="closeDrawer">Close</button>
      </div>
      <template v-if="drawerOpen">
        <SideNav :label="label" :items="items" :current="current" @navigate="closeDrawer" />
        <div v-if="$slots.footer" class="workspace-footer mt-6 border-t border-border pt-4"><slot name="footer" /></div>
      </template>
    </dialog>
    <div class="min-w-0 px-4 py-6 sm:px-8 lg:px-10 lg:py-8 2xl:px-14">
      <div :class="narrow ? 'mx-auto max-w-6xl' : ''"><slot /></div>
    </div>
  </div>
</template>

<script setup lang="ts">
// The frame both workspaces share, the admin's and the player's own: a side
// nav down the left (a drawer on a narrow screen), a footer under it (who is
// logged in), and the page beside it.
import { nextTick, onMounted, onUnmounted, ref, watch } from 'vue';
import { useRoute } from 'vue-router';
import SideNav, { type SideNavItem } from './SideNav.vue';

withDefaults(
  defineProps<{
    label: string;
    items: SideNavItem[];
    // The `to` of the item the page is under.
    current: string | null;
    // Keep the page to a reading width (the player's pages); the admin's
    // tables take the screen.
    narrow?: boolean;
  }>(),
  { narrow: false },
);

const route = useRoute();
const drawerId = `workspace-drawer-${Math.random().toString(36).slice(2, 8)}`;
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
function closeDrawer() {
  drawer.value?.close();
}
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
function onResize() {
  if (drawerOpen.value && window.innerWidth >= 1024) closeDrawer();
}
watch(() => route.fullPath, () => { if (drawerOpen.value) closeDrawer(); });
onMounted(() => window.addEventListener('resize', onResize));
onUnmounted(() => {
  window.removeEventListener('resize', onResize);
  if (drawerOpen.value) document.body.style.overflow = previousOverflow;
});
</script>
